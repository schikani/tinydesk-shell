# TinyDesk Shell

**A Unix-like shell for microcontrollers, with users, networking and SSH,
that also runs on a PC.** TinyDesk Shell (`tdsh`) is the
shell inside [TinyDesk](https://github.com/tinydesk-project/tinydesk), the
terminal desktop for microcontrollers, and works on its own too.

![TinyDesk Shell on an ESP32 serial console](docs/images/terminal.png)

*Actual serial-console capture of the standalone firmware.*

* Parser with variables, quoting, redirection, pipelines, command
  substitution, arithmetic and conditions; scripts (`.tdsh`, uScript 1.1.1).
* Line editor: Tab completion, history, arrows, Home/End, Ctrl+A/E/U/K/L/C;
  long lines wrap over several rows and stay editable.
* Files on LittleFS (`/fs`), `nano`, `write`; per-user homes with a sandbox.
* Users in NVS with salted, hashed passwords; `login`, `passwd`, boot user,
  physical-console recovery.
* Wi-Fi (per-user saved networks), W6100 Ethernet, LAN/Wi-Fi policy,
  `ping`, SNTP time, time zones.
* SSH/SFTP server (wolfSSH, per-device host key), FTP server, SMB2/3 mounts,
  the SD card at `/sd` (`sd mount`, FAT over SPI, optional mount at boot).
* **Board configuration**: pins for RS-485, Ethernet and SD come from a
  `key = value` file (`board` command, `tdsh_board.h`), not from the code.
* Hardware loopback tests (`hwtest`), heap and task information.
* A portable core with POSIX (Linux) and Windows host ports and regression
  tests; `tdsh.exe` runs the shell natively on Windows.

The C API uses the prefix `tdsh_` (`tdsh.h`, `tdsh_espidf.h`); scripts end in `.tdsh`.

## Install without building

This is a developer preview.

* **From the browser:** the [TinyDesk web installer](https://tinydesk-project.github.io/install/#shell/esp32c6)
  flashes TinyDesk Shell onto an ESP32-C6 or ESP32 (Chrome or Edge), and the
  [web terminal](https://tinydesk-project.github.io/console/) opens the board afterwards.
* **Files:** every [release](https://github.com/tinydesk-project/tinydesk-shell/releases) has a factory image
  per board (flash at offset 0 with esptool), the Linux program, the Windows
  program (`tdsh.exe`, run it in Windows Terminal) and `SHA256SUMS.txt`. On a
  PC the shell keeps its files apart from yours: in
  `~/.local/share/tdsh/rootfs` on Linux, `%LOCALAPPDATA%\tdsh\rootfs` on
  Windows.

A release is made by pushing a tag `v<VERSION>`: `.github/workflows/release.yml`
runs the host tests, builds both firmware projects and the Linux and Windows
programs, and
creates a draft pre-release with the files (`tools/make_release.py` packages
them; it runs locally too).

## Get the source

```bash
git clone https://github.com/tinydesk-project/tinydesk-shell.git
cd tinydesk-shell
```

## Build the firmware (ESP-IDF 5.3.1)

| Board | Project | Console |
| --- | --- | --- |
| ESP32-C6, 8 MB flash | the repository root | built-in USB Serial/JTAG |
| classic ESP32, 4 MB flash or more, PSRAM optional | `projects/esp32` | UART0 (the USB-UART chip), 115200 baud |

```bash
idf.py build                       # in the root, or in projects/esp32
idf.py -p PORT flash monitor
```

On Windows, `build_windows.cmd` runs an environment check first
(`build_windows.cmd -p COM7 flash monitor`). Keep the checkout in a short
folder such as `C:\src\tinydesk-shell`: ESP-IDF's nested build folders
otherwise run past Windows' 260-character path limit (`ninja: error:
mkdir(...): No such file or directory`). The ESP-IDF component manager
downloads the dependencies declared in
`ports/esp_idf/components/tdsh/idf_component.yml`.

You start as root; the factory root password (for SSH, FTP and `login`) is
`TinyDesk`: change it locally with `passwd` before remote access. SSH and FTP
refuse remote authentication while the factory root password remains. Then, for example:

```text
wifiadd MyNetwork mypassword  # save a network (per user)
wificonnect
ssh start
hello                         # the example application command (main/main.c)
```

### Your board's pins

RS-485 lines, a W6100 Ethernet chip and an SD card are configured with
`key = value` settings, not in the code. Copy the project's
`board.example.conf` to `board.conf` (ignored by git) and edit it before
building, or set them on the running board as root:

```text
board set eth.chip w6100
board set eth.miso 2
...
reboot
```

`board show` lists the settings. See the annotated
[board.example.conf](board.example.conf) for keys and defaults.

> The partition tables: ESP32-C6: application at `0x10000` (3 MB), LittleFS
`storage` at `0x310000`; classic ESP32: application at `0x10000` (2.5 MB),
LittleFS at `0x290000` (1.4 MB). The file system is formatted if it cannot be
mounted. Do not use `erase-flash` as a routine step.

## Embed it in your ESP-IDF application

Add `ports/esp_idf/components` to `EXTRA_COMPONENT_DIRS`, then:

```c
#include "tdsh_espidf.h"

extern const char board_conf[] asm("_binary_board_conf_start");   /* optional, EMBED_TXTFILES */

tdsh_espidf_config_t cfg = TDSH_ESP_IDF_CONFIG_DEFAULT();
cfg.hostname = "mydevice";
cfg.board_config = board_conf;            /* built-in pins; /fs/etc/board.conf overrides them */
ESP_ERROR_CHECK(tdsh_espidf_init(&cfg));
ESP_ERROR_CHECK(tdsh_espidf_start());   /* the ESP-IDF console: USB Serial/JTAG or UART */
```

Register your own commands with `tdsh_register_commands()`. See
`docs/EMBEDDING.md` and `examples/esp_idf/embed_in_app`.
TinyDesk embeds the shell this way and runs its console in a desktop window.
An integration that shares this console with a remote transport must call
`tdsh_console_mark_remote()` **before** accepting remote input and refuse the
connection if it returns false. This prevents remote use of physical password
recovery; successful takeover revokes physical trust until reboot.

## Host build and tests (Linux, WSL, Windows)

```bash
cmake -S . -B build-host -G Ninja -DTDSH_BUILD_HOST=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

On Windows add `-DCMAKE_C_COMPILER=gcc` (MinGW-w64); the program is
`build-host/tdsh_host.exe`. `docs/POSIX_HOST.md` and `docs/WINDOWS_HOST.md`
explain the host ports.

## Scripts

Put commands in a `.tdsh` file and run it with `tdsh run`. The script
language (uScript 1.1.1) has variables, single and double quotes, `$(…)`,
integer arithmetic `$((…))`, `if`/`elseif`/`else`/`endif`,
`while`/`endwhile`, `for … in`/`endfor` (with `*` and `?` globs),
`break`/`continue`, functions with arguments and return status
(`function`/`end`, `$1`, `$#`, `$?`), `test` and `[ … ]`, pipes,
`>`, `>>` and `<` redirection, and `&&`, `||` and `;`.

```sh
#!/bin/tdsh
function check
    if test -f "$1"
        echo "$1: $(cat $1)"
        return 0
    endif
    echo "$1 is missing"
    return 1
end

for F in ~/logs/*.txt
    check $F || echo "problem with $F"
endfor
N=$((3 * 4 + 1))
if $N >= 10
    echo "N is $N"
endif
```

```text
tdsh run ~/check.tdsh           # run and wait; $? is its status
tdsh run ~/tools                # a folder runs its main.tdsh
tdsh run ~/logger.tdsh --bg     # in the background
```

A script runs in a copy of the session, so its variables and `cd` do not
leak back. On the ESP32 each user's `~/.tdshrc.tdsh` runs when their local
console starts (for example `wificonnect`). The full reference, with every
condition form, the limits and a tested example: `docs/SCRIPTING.md`.
`tests/scripts/` holds self-checking test scripts.

## Community ports

Maintained by their authors, not built or tested here. Each port has its
own prefix for its names, commands and board keys
([STANDARDS.md](STANDARDS.md#port-prefix)):

| Port | Platform | Prefix | Notes |
| --- | --- | --- | --- |
| [TinyTang](https://github.com/aquasock/TinyTang) by [@aquasock](https://github.com/aquasock) | BL616 (FreeRTOS), Sipeed Tang Console 138K | `tang` | TinyDesk on the board's own BL616: the desktop is drawn over HDMI by the FPGA core and mirrored over USB CDC, with the SD card as filesystem and a Bluetooth LE keyboard and mouse; adds commands to load FPGA cores and ROMs, play music through the Tang-Phosphor core, and self-update the firmware |

## Documentation

| File | Content |
| --- | --- |
| `docs/SCRIPTING.md` | the `.tdsh` script language: syntax, conditions, loops, functions, pipes, limits |
| `docs/FUNCTIONS.md` | API and command inventory |
| `docs/ARCHITECTURE.md`, `docs/PORTING.md` | structure, porting to another platform |
| `docs/DEPENDENCIES.md` | component versions |
| `CHANGELOG.md` | changes per version |

## Licence

TinyDesk Shell is released under the [MIT licence](LICENSE).

Third-party components keep their own licences: see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and [NOTICE](NOTICE).
Firmware built with the ESP-IDF component includes wolfSSH and wolfSSL
(GPL-3.0) and is therefore distributed under the GPL-3.0 as a whole.

## Contributing

Issues and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
Run the host tests and build the standalone firmware before sending a
change, and keep board-specific pins out of the code (use board
configuration keys).

Set up formatting once:

```bash
pip install pre-commit
pre-commit install
```

After this, every commit formats the C files you changed with the right
clang-format version automatically. To format manually instead:
`pip install clang-format==16.0.6`, then `clang-format -i <files>`. If the
format check fails on your pull request, don't worry: I can fix it before
merging.
