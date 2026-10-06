#ifndef TDSH_H
#define TDSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tdsh_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TDSH_VERSION    "0.1.5"
#define USCRIPT_VERSION "1.1.1"

#define TDSH_MAX_LINE      512
#define TDSH_MAX_ARGS      48
#define TDSH_MAX_PATH      256
#define TDSH_MAX_REAL_PATH (TDSH_MAX_PATH * 2 + 96)
/* The variable table costs TDSH_MAX_VARS * (1 + TDSH_VAR_NAME_MAX +
 * TDSH_VAR_VALUE_MAX) bytes in every session, including each script's copy.
 * Buffer sizes include the terminating NUL.  An override must apply to every
 * component using tdsh.h (core, ports and consumers such as TinyDesk), since
 * these limits change tdsh_session_t's layout. */
#ifndef TDSH_MAX_VARS
#define TDSH_MAX_VARS 64
#endif
#ifndef TDSH_VAR_NAME_MAX
#define TDSH_VAR_NAME_MAX 32
#endif
#ifndef TDSH_VAR_VALUE_MAX
#define TDSH_VAR_VALUE_MAX 256
#endif
#define TDSH_USERNAME_MAX 32

/* Script files use the .tdsh extension. Each user's startup script, run
 * when the physical console starts as that user: */
#define TDSH_SCRIPT_EXT       ".tdsh"
#define TDSH_STARTUP_FILE     ".tdshrc.tdsh"
#define TDSH_HOSTNAME_MAX     32
#define TDSH_MAX_COMMANDS     128
#define TDSH_COPY_BUFFER_SIZE 4096
/* Bytes requested for each script worker's stack, in addition to its
 * session copy and runtime allocations.  Ports may impose their own cap. */
#ifndef TDSH_SCRIPT_TASK_STACK
#define TDSH_SCRIPT_TASK_STACK 32768
#endif
#define TDSH_SCRIPT_TASK_PRIORITY 4

/* Compatibility defaults used by the ESP-IDF reference port. The portable
 * path resolver uses config.fs_root instead of this macro. */
#ifndef TDSH_MOUNT_POINT
#define TDSH_MOUNT_POINT "/fs"
#endif
#ifndef TDSH_PARTITION_LABEL
#define TDSH_PARTITION_LABEL "storage"
#endif

#define TDSH_CMD_ROOT_ONLY   (1u << 0)
#define TDSH_CMD_INTERACTIVE (1u << 1)
#define TDSH_CMD_BG_ALLOWED  (1u << 2)

#define TDSH_TERM_CAP_ANSI       (1u << 0)
#define TDSH_TERM_CAP_COLOR      (1u << 1)
#define TDSH_TERM_CAP_FULLSCREEN (1u << 2)

typedef int tdsh_status_t;

typedef struct
{
    bool used;
    char name[TDSH_VAR_NAME_MAX];
    char value[TDSH_VAR_VALUE_MAX];
} tdsh_var_t;

typedef struct tdsh_session
{
    char username[TDSH_USERNAME_MAX];
    char hostname[TDSH_HOSTNAME_MAX];
    char cwd[TDSH_MAX_PATH];
    char home[TDSH_MAX_PATH];
    tdsh_var_t vars[TDSH_MAX_VARS];
    int last_status;
    void *script_runtime;
    void *user_context;
    bool interactive;
    uint32_t terminal_caps;
    bool logout_requested;
} tdsh_session_t;

typedef int (*tdsh_command_fn_t)(tdsh_session_t *session,
                                 int argc,
                                 char **argv);

typedef struct
{
    const char *name;
    const char *usage;
    const char *help;
    tdsh_command_fn_t fn;
    uint32_t flags;
} tdsh_command_t;

typedef bool (*tdsh_path_translate_fn_t)(void *context,
                                         const char *logical_path,
                                         char *real_out,
                                         size_t real_out_size);

typedef struct
{
    const char *hostname;
    const char *default_user;
    const char *fs_root;
    size_t history_length;
    const tdsh_platform_api_t *platform;
    tdsh_path_translate_fn_t path_translate;
    void *path_translate_context;
} tdsh_core_config_t;

#define TDSH_CORE_CONFIG_DEFAULT()      \
    {                                   \
        .hostname = "device",           \
        .default_user = "root",         \
        .fs_root = ".",                 \
        .history_length = 50,           \
        .platform = NULL,               \
        .path_translate = NULL,         \
        .path_translate_context = NULL, \
    }

typedef struct
{
    size_t live_blocks;
    size_t live_bytes;
    size_t peak_blocks;
    size_t peak_bytes;
    size_t total_allocations;
    size_t failed_allocations;
} tdsh_memory_stats_t;

/* Core lifecycle. Registration is intentionally allocation-free and should be
 * completed before interactive/remote sessions start. */
int tdsh_core_init(const tdsh_core_config_t *config);
void tdsh_core_reset(void);
const tdsh_core_config_t *tdsh_core_config(void);
const char *tdsh_platform_name(void);

int tdsh_session_init(tdsh_session_t *session,
                      const char *username,
                      bool interactive);
int tdsh_session_clone(tdsh_session_t *dst,
                       const tdsh_session_t *src,
                       bool interactive);

/* Command SDK. Command descriptors and their strings must remain valid for the
 * lifetime of the registry (normally static const objects). */
int tdsh_register_command(const tdsh_command_t *command);
int tdsh_register_commands(const tdsh_command_t *commands, size_t count);
const tdsh_command_t *tdsh_commands_get(size_t *count);
const tdsh_command_t *tdsh_command_find(const char *name);
int tdsh_execute_argv(tdsh_session_t *session, int argc, char **argv);
int tdsh_execute_line(tdsh_session_t *session, const char *line);

/* Portable built-in command set: filesystem, variables, tests, scripting,
 * uptime/platform info, and shell conveniences. */
int tdsh_register_core_builtins(void);

/* Variables. */
const char *tdsh_var_get(tdsh_session_t *session, const char *name);
int tdsh_var_set(tdsh_session_t *session, const char *name, const char *value);
int tdsh_var_unset(tdsh_session_t *session, const char *name);

/* Path sandbox / virtual root. */
int tdsh_path_normalize(tdsh_session_t *session,
                        const char *input,
                        char *logical_out,
                        size_t logical_out_size);
int tdsh_path_to_real(tdsh_session_t *session,
                      const char *input,
                      char *real_out,
                      size_t real_out_size,
                      char *logical_out,
                      size_t logical_out_size);

/* Shared parser helpers used by uScript. */
int tdsh_parse_words(tdsh_session_t *session,
                     const char *input,
                     char *storage,
                     size_t storage_size,
                     char **argv,
                     int *argc_out);
int tdsh_eval_int_expr(tdsh_session_t *session,
                       const char *expr,
                       int64_t *value_out);

/* uScript. Explicit scripts run in an isolated session clone. On platforms
 * providing worker_run, the ESP-style large dedicated script stack is kept. */
int tdsh_run_script(tdsh_session_t *session,
                    const char *path,
                    bool background);
int tdsh_run_script_in_session(tdsh_session_t *session,
                               const char *path);
bool tdsh_script_special_var(tdsh_session_t *session,
                             const char *name,
                             char *out,
                             size_t out_size);
bool tdsh_script_try_function(tdsh_session_t *session,
                              int argc,
                              char **argv,
                              int *status_out);

/* Platform helpers used by portable modules. */
uint64_t tdsh_monotonic_ms(void);
void tdsh_sleep_ms(uint32_t ms);
void tdsh_yield(void);
int tdsh_random_bytes(void *buffer, size_t length);

/* Core-owned allocations. These wrappers are used by parser/uScript and keep
 * live/peak accounting so host leak tests can assert zero outstanding blocks. */
void *tdsh_malloc(size_t size);
void *tdsh_calloc(size_t count, size_t size);
void *tdsh_realloc(void *ptr, size_t size);
void tdsh_free(void *ptr);
char *tdsh_strdup(const char *text);
void tdsh_memory_get_stats(tdsh_memory_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* TDSH_H */
