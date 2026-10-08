#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tdsh_posix.h"
#include "tdsh_terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 8192  /* fallback for macOS */
#endif

static tdsh_posix_config_t s_cfg;
static tdsh_session_t s_session;
static char s_hostname[TDSH_HOSTNAME_MAX];
static char s_user[TDSH_USERNAME_MAX];
static char s_fs_root[TDSH_MAX_REAL_PATH];
static char s_host_home[TDSH_MAX_REAL_PATH];
static bool s_color_enabled;

static uint64_t posix_monotonic_ms(void *context)
{
    (void)context;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void posix_sleep_ms(void *context, uint32_t ms)
{
    (void)context;
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
    {
    }
}

static void posix_yield(void *context)
{
    (void)context;
    sched_yield();
}

static int posix_random_bytes(void *context, void *buffer, size_t length)
{
    (void)context;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -errno;
    unsigned char *p = buffer;
    size_t done = 0;
    while (done < length)
    {
        ssize_t n = read(fd, p + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            int rc = n < 0 ? -errno : -EIO;
            close(fd);
            return rc;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

typedef struct
{
    tdsh_worker_fn_t worker;
    tdsh_worker_cleanup_fn_t cleanup;
    void *arg;
    int result;
    bool self_free;
} posix_worker_ctx_t;

static void *posix_worker_entry(void *opaque)
{
    posix_worker_ctx_t *ctx = opaque;
    ctx->result = ctx->worker(ctx->arg);
    if (ctx->cleanup)
        ctx->cleanup(ctx->arg);
    if (ctx->self_free)
        free(ctx);
    return NULL;
}

static int posix_worker_run(void *context,
                            const char *name,
                            size_t stack_bytes,
                            int priority,
                            bool background,
                            tdsh_worker_fn_t worker,
                            void *arg,
                            tdsh_worker_cleanup_fn_t cleanup,
                            int *result_out)
{
    (void)context;
    (void)name;
    (void)priority;
    if (!worker)
        return -EINVAL;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return -errno;
    if (stack_bytes > 0)
    {
        size_t desired = stack_bytes < (size_t)PTHREAD_STACK_MIN ? (size_t)PTHREAD_STACK_MIN : stack_bytes;
        (void)pthread_attr_setstacksize(&attr, desired);
    }

    pthread_t thread;
    if (background)
    {
        posix_worker_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx)
        {
            pthread_attr_destroy(&attr);
            return -ENOMEM;
        }
        ctx->worker = worker;
        ctx->cleanup = cleanup;
        ctx->arg = arg;
        ctx->self_free = true;
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&thread, &attr, posix_worker_entry, ctx);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            free(ctx);
            return -rc;
        }
        return 0;
    }

    posix_worker_ctx_t ctx = {
        .worker = worker,
        .cleanup = cleanup,
        .arg = arg,
        .result = 1,
        .self_free = false,
    };
    int rc = pthread_create(&thread, &attr, posix_worker_entry, &ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return -rc;
    rc = pthread_join(thread, NULL);
    if (rc != 0)
        return -rc;
    if (result_out)
        *result_out = ctx.result;
    return 0;
}

static const tdsh_platform_api_t s_platform = {
    .name = "posix/pthread",
    .context = NULL,
    .monotonic_ms = posix_monotonic_ms,
    .sleep_ms = posix_sleep_ms,
    .yield = posix_yield,
    .random_bytes = posix_random_bytes,
    .malloc_fn = NULL,
    .calloc_fn = NULL,
    .realloc_fn = NULL,
    .free_fn = NULL,
    .worker_run = posix_worker_run,
};

const tdsh_platform_api_t *tdsh_posix_platform(void)
{
    return &s_platform;
}


static int mkdir_p_host(const char *path)
{
    if (!path || !*path)
        return -EINVAL;
    char tmp[TDSH_MAX_REAL_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -ENAMETOOLONG;
    for (char *p = tmp + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -errno;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

static bool posix_path_translate(void *context, const char *logical, char *real, size_t cap)
{
    (void)context;
    if (!s_cfg.map_default_user_home_to_host_home || !s_host_home[0] || !logical)
        return false;
    char prefix[TDSH_MAX_PATH];
    int n = strcmp(s_user, "root") == 0
                ? snprintf(prefix, sizeof(prefix), "/root")
                : snprintf(prefix, sizeof(prefix), "/home/%s", s_user);
    if (n < 0 || (size_t)n >= sizeof(prefix))
        return false;
    size_t plen = strlen(prefix);
    if (strcmp(logical, prefix) == 0)
    {
        return snprintf(real, cap, "%s", s_host_home) >= 0 && strlen(s_host_home) < cap;
    }
    if (strncmp(logical, prefix, plen) == 0 && logical[plen] == '/')
    {
        n = snprintf(real, cap, "%s%s", s_host_home, logical + plen);
        return n >= 0 && (size_t)n < cap;
    }
    return false;
}

const char *tdsh_posix_real_fs_root(void)
{
    return s_fs_root;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -errno;
}

static int ensure_tree(const char *root, const char *user)
{
    char path[TDSH_MAX_REAL_PATH + TDSH_USERNAME_MAX + 16];
    if (mkdir_one(root) != 0)
        return -errno;
    snprintf(path, sizeof(path), "%s/root", root);
    if (mkdir_one(path) != 0)
        return -errno;
    snprintf(path, sizeof(path), "%s/home", root);
    if (mkdir_one(path) != 0)
        return -errno;
    snprintf(path, sizeof(path), "%s/tmp", root);
    if (mkdir_one(path) != 0)
        return -errno;
    snprintf(path, sizeof(path), "%s/etc", root);
    if (mkdir_one(path) != 0)
        return -errno;
    if (strcmp(user, "root") != 0)
    {
        snprintf(path, sizeof(path), "%s/home/%s", root, user);
        if (mkdir_one(path) != 0)
            return -errno;
    }
    return 0;
}

int tdsh_posix_init(const tdsh_posix_config_t *config)
{
    if (!config)
        return -EINVAL;
    memset(&s_cfg, 0, sizeof(s_cfg));

    const struct passwd *pw = getpwuid(geteuid());
    const char *user = config->default_user && config->default_user[0] ? config->default_user : (pw && pw->pw_name ? pw->pw_name : getenv("USER"));
    if (!user || !*user)
        user = "developer";
    const char *home = config->host_home && config->host_home[0] ? config->host_home : (pw && pw->pw_dir ? pw->pw_dir : getenv("HOME"));
    if (!home || !*home)
        home = ".";

    const char *host = config->hostname;
    if (!host || !*host || strcmp(host, "localhost") == 0)
    {
        if (gethostname(s_hostname, sizeof(s_hostname) - 1) != 0)
            snprintf(s_hostname, sizeof(s_hostname), "localhost");
        s_hostname[sizeof(s_hostname) - 1] = '\0';
        host = s_hostname;
    }
    else
        snprintf(s_hostname, sizeof(s_hostname), "%s", host);
    snprintf(s_user, sizeof(s_user), "%s", user);
    snprintf(s_host_home, sizeof(s_host_home), "%s", home);

    if (config->fs_root && config->fs_root[0])
        snprintf(s_fs_root, sizeof(s_fs_root), "%s", config->fs_root);
    else
    {
        int n = snprintf(s_fs_root, sizeof(s_fs_root), "%s/.local/share/tdsh/rootfs", home);
        if (n < 0 || (size_t)n >= sizeof(s_fs_root))
            return -ENAMETOOLONG;
    }
    int rc = mkdir_p_host(s_fs_root);
    if (rc)
        return rc;

    s_cfg = *config;
    s_cfg.hostname = s_hostname;
    s_cfg.default_user = s_user;
    s_cfg.fs_root = s_fs_root;
    s_cfg.host_home = s_host_home;
    /* Explicit fs_root without an explicit host_home means sandbox-only. This keeps tests/CI isolated. */
    if (config->fs_root && config->fs_root[0] && (!config->host_home || !config->host_home[0]))
        s_cfg.map_default_user_home_to_host_home = false;
    rc = ensure_tree(s_fs_root, s_user);
    if (rc)
        return rc;

    const char *term = getenv("TERM");
    s_color_enabled = isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb") != 0) && getenv("NO_COLOR") == NULL;

    tdsh_core_config_t core = TDSH_CORE_CONFIG_DEFAULT();
    core.hostname = s_hostname;
    core.default_user = s_user;
    core.fs_root = s_fs_root;
    core.platform = &s_platform;
    core.path_translate = posix_path_translate;
    core.path_translate_context = NULL;
    rc = tdsh_core_init(&core);
    if (rc)
        return rc;
    if (config->register_core_builtins)
    {
        rc = tdsh_register_core_builtins();
        if (rc)
            return rc;
    }
    if (config->register_posix_commands)
    {
        rc = tdsh_posix_register_commands();
        if (rc)
            return rc;
    }
    rc = tdsh_session_init(&s_session, s_user, true);
    if (rc)
        return rc;
    s_session.terminal_caps = TDSH_TERM_CAP_ANSI | (s_color_enabled ? TDSH_TERM_CAP_COLOR : 0);
    return 0;
}

static void build_prompt(const tdsh_session_t *s, char *out, size_t cap)
{
    const char marker = strcmp(s->username, "root") == 0 ? '#' : '$';
    char display[TDSH_MAX_PATH];
    if (strcmp(s->cwd, s->home) == 0)
        snprintf(display, sizeof(display), "~");
    else if (strncmp(s->cwd, s->home, strlen(s->home)) == 0 && s->cwd[strlen(s->home)] == '/')
        snprintf(display, sizeof(display), "~%s", s->cwd + strlen(s->home));
    else
        snprintf(display, sizeof(display), "%s", s->cwd);
    if (s_color_enabled)
        snprintf(out, cap, "\033[1;32m%s@%s\033[0m:\033[1;34m%s\033[0m%c ", s->username, s->hostname, display, marker);
    else
        snprintf(out, cap, "%s@%s:%s%c ", s->username, s->hostname, display, marker);
}

static int posix_terminal_read_byte(void *context, uint8_t *byte_out)
{
    (void)context;
    if (!byte_out)
        return -EINVAL;
    for (;;)
    {
        ssize_t n = read(STDIN_FILENO, byte_out, 1U);
        if (n == 1)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            return -EIO;
        return -errno;
    }
}

static int posix_terminal_columns(void *context)
{
    (void)context;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        return 0;
    return ws.ws_col;
}

static int posix_terminal_read_byte_timeout(void *context, uint8_t *byte_out, unsigned timeout_ms)
{
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    for (;;)
    {
        int n = poll(&p, 1, (int)timeout_ms);
        if (n > 0)
            return posix_terminal_read_byte(context, byte_out);
        if (n == 0)
            return -ETIMEDOUT;
        if (errno != EINTR)
            return -errno;
    }
}

static int posix_terminal_write_bytes(void *context, const void *data, size_t length)
{
    (void)context;
    const uint8_t *p = data;
    size_t done = 0U;
    while (done < length)
    {
        ssize_t n = write(STDOUT_FILENO, p + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n < 0 ? -errno : -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int posix_readline(tdsh_session_t *session,
                          const char *prompt,
                          char *line,
                          size_t capacity)
{
    const tdsh_terminal_io_t io = {
        .context = NULL,
        .read_byte = posix_terminal_read_byte,
        .write_bytes = posix_terminal_write_bytes,
        .columns = posix_terminal_columns,
        .read_byte_timeout = posix_terminal_read_byte_timeout,
    };

    struct termios previous;
    bool changed = false;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &previous) == 0)
    {
        struct termios raw = previous;
        cfmakeraw(&raw);
        raw.c_oflag |= OPOST;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
            changed = true;
    }

    int rc = tdsh_terminal_readline(session, &io, prompt, line, capacity);

    if (changed)
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
    return rc;
}

int tdsh_posix_run_interactive(void)
{
    char line[TDSH_MAX_LINE + 2];
    char prompt[TDSH_MAX_PATH + TDSH_HOSTNAME_MAX + TDSH_USERNAME_MAX + 64];
    printf("TinyDesk Shell %s - portable POSIX host\nType 'help' for commands.\n", TDSH_VERSION);
    printf("Virtual root: %s\n", s_fs_root);
    if (s_cfg.map_default_user_home_to_host_home)
    {
        printf("WARNING: TinyDesk Shell home %s is mapped to host home %s\n", s_session.home, s_host_home);
    }
    else
    {
        char real_home[TDSH_MAX_REAL_PATH];
        if (tdsh_path_to_real(&s_session, "~", real_home, sizeof(real_home), NULL, 0) == 0)
        {
            printf("Isolated TinyDesk Shell home: %s\n", real_home);
        }
    }
    printf("Terminal editing: TAB/history/arrows/Home/End/Delete enabled\n");
    printf("Colors: %s (set NO_COLOR=1 to disable)\n\n", s_color_enabled ? "enabled" : "disabled");

    for (;;)
    {
        build_prompt(&s_session, prompt, sizeof(prompt));
        int n = posix_readline(&s_session, prompt, line, sizeof(line));
        if (n < 0)
            break;
        if (strcmp(line, "exit") == 0)
            break;
        if (line[0])
            (void)tdsh_execute_line(&s_session, line);
    }
    return 0;
}
