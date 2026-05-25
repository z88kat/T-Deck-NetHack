# NetHack 5.0 on the LilyGo T-Deck

A port of [NetHack 5.0](https://github.com/NetHack/NetHack) to the
[LilyGo T-Deck](https://lilygo.cc/products/t-deck/) handheld — a 320x240
colour TFT, BlackBerry-style QWERTY keyboard, microSD slot, and trackball
on an ESP32-S3 with 16 MB flash and 8 MB PSRAM.

The upstream NetHack engine runs as a static library (`libnh.a`) on
Xtensa; an ESP-IDF application in [lilygo/](lilygo/) provides display,
keyboard, SD-card, and power-management glue.  See
[TDECK_PORT_PLAN.md](TDECK_PORT_PLAN.md) for the original design.

## Status

| Phase | Description | State |
|------:|-------------|------|
| 0 | Host build of `libnh.a` on macOS | ✅ |
| 1 | Cross-compile `libnh.a` + `hacklib.a` + `lua548.a` for Xtensa | ✅ |
| 2 | ESP-IDF skeleton: shim windowport, FreeRTOS task | ✅ |
| 3a | ST7789 display: 8x8 ASCII glyph rendering | ✅ |
| 3b | Map rendering with 40-col viewport tracking the player | ✅ |
| 3c | Menus (PICK_NONE/ONE/ANY), extcmd, yn prompts, colour glyphs | ✅ |
| 3c+ | Message ring buffer, `--More--`, scrollback (Ctrl+P / `~`) | ✅ |
| 4 | I²C QWERTY keyboard | ✅ |
| 5 | SD card mount, save/restore round-trip, auto-wipe on corruption, FAT durability via `fsync` | ✅ |
| 6a | LEDC PWM backlight, idle dim/off | ✅ |
| 6b | Trackball — dropped (hardware too unreliable to be useful) | ✗ |
| 6c | Sound — dropped (not used by the gameplay loop) | ✗ |

You can boot, name your character, see the dungeon, walk around with
`hjkl`, fight monsters, pick up items, manage your inventory, save the
game with `S`, and resume after a power cycle.

## Controls

The T-Deck has no dedicated ESC or Ctrl key; the shim remaps:

| Want | Press | Notes |
|------|-------|-------|
| ESC / cancel | `` ` `` (backtick) or Backspace | Either works |
| Ctrl+P (previous messages) | `~` (Shift+backtick) | Opens scrollback overlay |
| `#` extcmd | `#` (Sym+Q on most T-Deck firmwares) | Type the command name, Enter |
| `--More--` | any key | Triggered automatically when text doesn't fit |
| Menu navigation | letter accelerators, `j`/`k` scroll, Enter confirm, ESC cancel | |

Everything else is stock NetHack: `hjkl` movement, `y/u/b/n` diagonals,
`i` inventory, `,` pick up, `s` search, `S` save & quit, `?` help.

## Hardware requirements

- **LilyGo T-Deck** (ESP32-S3FN16R8 variant: 16 MB flash, 8 MB OPI PSRAM).
- USB-C cable for flashing and serial console.
- **microSD card, FAT32-formatted** — required for save/restore.  Inserted
  before boot; the firmware mounts it at `/sdcard` and creates
  `/sdcard/save/`.  If no card is present saves are disabled but the game
  still plays.

## Quick start (macOS)

```bash
# One-off: install ESP-IDF v5.3.x and patch the Python dep checker (see
# "Known ESP-IDF Python issue" below).
. ~/esp/esp-idf/export.sh

# Build + flash + monitor with one command:
./build_lily.sh
```

The [build_lily.sh](build_lily.sh) script handles the host build, Xtensa
cross-compile, ESP-IDF firmware build, port autodetection, flash, and
serial monitor.  Run `./build_lily.sh --help` for options.

## Software requirements

### Host (macOS)

- Xcode Command Line Tools (`xcode-select --install`) — provides clang, make.
- Homebrew packages:
  ```bash
  brew install cmake ninja dfu-util
  ```
- Python 3.8+ (system Python on macOS 12+ works).

### ESP-IDF v5.3.x

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.2 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf && ./install.sh esp32s3
```

After install, source the env at the start of every shell session:

```bash
. ~/esp/esp-idf/export.sh
```

#### Known ESP-IDF Python issue

ESP-IDF v5.3.2's `tools/check_python_dependencies.py` mis-resolves package
names containing dots (e.g. `ruamel.yaml.clib`) on Python 3.9's stdlib
`importlib.metadata`.  If you see `ESP_ERR_INVALID_ARG` or "Package was not
found and is required by the application: ruamel.yaml.clib", patch the
script to PEP-503-normalise lookups:

```python
# in tools/check_python_dependencies.py, replace the version()/requires()
# imports with wrappers that retry with normalised names:
def _normalise(name): return re.sub(r'[-_.]+', '-', name).lower()
def get_version(name):
    try:    return _stdlib_version(name)
    except PackageNotFoundError: return _stdlib_version(_normalise(name))
def requires(name):
    try:    return _stdlib_requires(name)
    except PackageNotFoundError: return _stdlib_requires(_normalise(name))
```

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Host (macOS)                                                    │
│  ┌──────────────────┐    ┌──────────────────┐                    │
│  │ make WANT_LIBNH=1│ →  │ dat/nhdat        │   (NetHack data)   │
│  │   all            │    │ targets/esp32s3/ │                    │
│  │ make esp32s3     │ →  │   libnh.a        │   (Xtensa engine)  │
│  │                  │    │   hacklib.a      │                    │
│  │                  │    │   lua548.a       │                    │
│  └──────────────────┘    └────────┬─────────┘                    │
│                                   │                              │
│                       cmake import via lilygo/components/nethack │
│                                   ▼                              │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │ lilygo/  (ESP-IDF firmware project)                      │    │
│  │   main/app_main.c            -- boot, mount, splash      │    │
│  │   main/shim_callback.c       -- windowport bridge        │    │
│  │   components/tdeck_display   -- ST7789 + LEDC backlight  │    │
│  │   components/tdeck_keyboard  -- I²C QWERTY               │    │
│  │   components/tdeck_sdcard    -- SD over shared SPI bus   │    │
│  └──────────────────┬───────────────────────────────────────┘    │
└─────────────────────┼──────────────────────────────────────────  ┘
                      │   idf.py flash
                      ▼
              ┌────────────────────┐
              │  LilyGo T-Deck     │
              │  ESP32-S3 + LCD    │
              │  + QWERTY + microSD│
              └────────────────────┘
```

## Build (manual steps)

`build_lily.sh` does all three stages; this section explains what it does.

### 1. Host build (one-time per engine change)

Produces `util/makedefs`, `util/dlb`, `dat/nhdat`, and a host `src/libnh.a`
to validate the engine compiles.

```bash
cd sys/unix && ./setup.sh hints/macOS.500 && cd ../..
make fetch-lua            # only on first ever build
make WANT_LIBNH=1 all
```

### 2. Xtensa cross-compile (one-time per engine change)

Produces `targets/esp32s3/{libnh,hacklib,lua548}.a` — the Xtensa-ELF
static archives the ESP-IDF project consumes.

```bash
. ~/esp/esp-idf/export.sh
touch src/tile.c          # avoids host-vs-target CFLAGS mix in tile regen
make esp32s3
```

### 3. ESP-IDF firmware build (every iteration)

```bash
cd lilygo
. ~/esp/esp-idf/export.sh
idf.py build
```

The build:

- imports `../targets/esp32s3/*.a` via `components/nethack/`
- routes libnh's large `.bss` (mons[], objects[], gg, svl, glyphmap, ...) into
  PSRAM via `components/nethack/linker.lf` and the `NH_EXTRAM` section
  attribute (see [include/config.h](include/config.h))
- builds a SPIFFS image from `dat/nhdat` and flashes it to the `nhdat`
  partition at `0x410000`

Total firmware ≈ 2.3 MB + 3 MB nhdat partition.

## Flash + run

```bash
cd lilygo
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem* flash monitor
```

In the serial monitor, **press `Ctrl+]`** to quit (Ctrl+C does not work).
On German Mac keyboards `]` is `Alt+5` (or `Alt+9` on some layouts); if
that's awkward, `pkill -f idf_monitor.py` from another shell works too.

### Boot sequence

1. ESP-IDF boot log → PSRAM init → `Calling app_main()`.
2. Splash: centered "NetHack 5.0" on black for ~1.2 s.
3. Keyboard init, nhdat mount at `/nethack`, SD mount at `/sdcard`.
4. NetHack starts; if a save exists on the SD it resumes, otherwise the
   tutorial menu pops up.

## ESP-32 specific engine patches

Several engine features had to be conditionally compiled out for the
cross build.  All wrapped in `#ifdef CROSS_TO_ESP32S3` or its negation
so the host build is unaffected:

| File | Change | Reason |
|------|--------|--------|
| [include/config.h](include/config.h) | `#undef COMPRESS`, `#undef INSURANCE` for cross | No `fork()/exec()` on ESP-IDF; `INSURANCE` makes `dorecover` look for a lock file a clean save+reboot deletes |
| [src/sfstruct.c](src/sfstruct.c) | `#undef USE_BUFFERING` for cross | `fdopen()` on FATFS VFS fds doesn't flush reliably; fall back to direct `write()` |
| [src/files.c](src/files.c) | `fsync()` before `close()` in `close_nhfile` | Force FATFS to commit pending sectors to the SD card before the descriptor is closed |
| [src/Makefile](src/Makefile) | Skip `tile.c` regen for cross | The util/ sub-make uses host CC with target `-mlongcalls` |
| [sys/libnh/libnhmain.c](sys/libnh/libnhmain.c) | `nh_set_savedir()` redirects SAVE / LEVEL / BONES / LOCK prefixes | Lets the shim point writable state at `/sdcard` while read-only data stays on SPIFFS |

The shim ([lilygo/main/shim_callback.c](lilygo/main/shim_callback.c)) adds
two safety nets:

- **Auto-wipe on corrupt save**: if NetHack's `shim_raw_print` emits
  "Error reading level file", "Error restoring old game", or
  "Cannot open save file", the shim wipes `/sdcard/save/` and
  `esp_restart()`s.  Bricked saves never persist past one boot.
- **Auto-reboot on `shim_exit_nhwindows`**: ESP-IDF newlib's `_exit` is
  unimplemented; the shim catches NetHack's exit call, paints the
  farewell text, waits 1.5 s, then `esp_restart()`s back to the splash.

## Hardware pin map

For the standard LilyGo T-Deck.  Override the `#define`s in the relevant
component header if your variant differs.

| Function | Pin | Source file |
|----------|-----|-------------|
| Peripheral power enable (LCD / keyboard / SD) | GPIO 10 | `tdeck_display.c` |
| LCD: SPI SCLK | GPIO 40 | `tdeck_display.h` |
| LCD: SPI MOSI | GPIO 41 | `tdeck_display.h` |
| LCD: SPI MISO (shared with SD) | GPIO 38 | `tdeck_display.c` |
| LCD: CS | GPIO 12 | `tdeck_display.h` |
| LCD: DC | GPIO 11 | `tdeck_display.h` |
| LCD: backlight (LEDC PWM) | GPIO 42 | `tdeck_display.h` |
| SD card: CS | GPIO 39 | `tdeck_sdcard.h` |
| SD card: MISO | GPIO 38 | `tdeck_sdcard.h` |
| Keyboard: I²C SDA | GPIO 18 | `tdeck_keyboard.h` |
| Keyboard: I²C SCL | GPIO 8 | `tdeck_keyboard.h` |
| Keyboard: INT (polled, not yet IRQ) | GPIO 46 | `tdeck_keyboard.h` |
| Keyboard: I²C address | `0x55` | `tdeck_keyboard.h` |

## Memory layout (8 MB PSRAM, 16 MB flash)

Flash partitions (see [lilygo/partitions.csv](lilygo/partitions.csv)):

```
0x000000  bootloader
0x008000  partition table
0x009000  nvs              (24 KB)
0x00f000  phy_init         (4 KB)
0x010000  factory app      (4 MB)
0x410000  nhdat            (3 MB SPIFFS, mounted at /nethack)
0x710000  storage          (1 MB SPIFFS, reserved for future use)
```

NetHack save / level / bones files live on the microSD at `/sdcard/save/`
and `/sdcard/`, not in flash — wear on the internal NOR is the reason.

RAM:

- `.dram0.bss` ≈ 2.4 KB
- `.dram0.data` ≈ 34 KB
- `.iram0.text` ≈ 54 KB
- `.ext_ram.bss` ≈ 407 KB (NetHack engine globals — PSRAM)
- `.flash.text` ≈ 1.6 MB
- `.flash.rodata` ≈ 521 KB

Free PSRAM heap available at runtime ≈ 7.7 MB.

## Power management

The display backlight is driven by LEDC PWM on `LEDC_CHANNEL_0` at
~5 kHz, 8-bit resolution.  An idle watchdog in `app_main.c` polls the
keyboard activity timestamp every 500 ms and adjusts:

- 0–30 s since last key → full brightness (255)
- 30–90 s → dim (≈ 12 %)
- > 90 s → off (0)

Any keypress restores full brightness within 500 ms.  The CPU keeps
running (no light/deep sleep yet); the game state is preserved across
the dim.

## Project structure

In-tree additions for the ESP32-S3 port:

```
T-Deck-NetHack/
├── README-lilly.md                        # this file
├── build_lily.sh                          # one-shot build + flash + monitor
├── sys/esp32s3/esp32sys.c                 # POSIX shim stubs (fork, getpwuid, …)
├── sys/libnh/libnhmain.c                  # nh_glyph_info_char/_color,
│                                          #   nh_set_savedir, fqn_prefix init,
│                                          #   nh_anything_size / _is_zero,
│                                          #   nh_menu_alloc_list / _set_item,
│                                          #   nh_lookup_ext_cmd
├── sys/unix/hints/esp32s3.500             # cross-compile hints
├── sys/unix/hints/include/                # CFLAGS / SYSSRC / SYSOBJ blocks
├── include/config.h                       # NH_EXTRAM macro,
│                                          #   COMPRESS/INSURANCE disabled for cross
├── include/decl.h                         # …
├── src/{decl,display,monst,objects}.c     # NH_EXTRAM on big globals
├── src/sfstruct.c                         # USE_BUFFERING disabled for cross
├── src/files.c                            # fsync() before close_nhfile()
├── src/Makefile                           # skip tile.c regen for cross
└── lilygo/                                # ESP-IDF firmware project
    ├── CMakeLists.txt                     # stages nhdat from ../dat/
    ├── partitions.csv
    ├── sdkconfig.defaults                 # PSRAM, BSS-in-PSRAM
    ├── main/
    │   ├── app_main.c                     # boot, splash, mount, idle watchdog
    │   ├── shim_callback.c                # windowport bridge
    │   └── CMakeLists.txt
    └── components/
        ├── nethack/                       # IMPORTS Xtensa .a files
        │   ├── CMakeLists.txt
        │   ├── linker.lf                  # routes libnh's .ext_ram.bss to PSRAM
        │   └── include/nethack.h          # public API for the shim
        ├── tdeck_display/                 # ST7789 + LEDC backlight
        ├── tdeck_keyboard/                # I²C QWERTY (poll @ 0x55)
        └── tdeck_sdcard/                  # SD over shared SPI2 bus
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `idf.py` complains `Package was not found and is required: ruamel.yaml.clib` | ESP-IDF v5.3.2 + stdlib `importlib.metadata` PEP 503 bug | Patch `check_python_dependencies.py` (see "Software requirements" above) |
| `make esp32s3` fails with `clang: error: unknown argument '-mlongcalls'` | Top-level make tried to regen `src/tile.c` via the host util/ sub-make | `touch src/tile.c` before running, or use `build_lily.sh` which does this for you |
| `Cache disabled but cached memory region accessed` panic in `decl_globals_init` | NetHack's `.bss` lands inside `.ext_ram.dummy` reservation | Verify `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` in `lilygo/sdkconfig` |
| `nhdat mount failed: ESP_ERR_INVALID_ARG (0x102)` | VFS path > 15 chars | HACKDIR / mount path must be ≤ 15 chars (currently `/nethack`) |
| `nhl_loadlua: Error opening (nhlib.lua)` | nhdat partition not flashed | Re-flash; `make WANT_LIBNH=1 all` to rebuild `dat/nhdat` |
| `tdeck_sdcard: mount failed` | No card inserted, unformatted, or non-FAT32 | Insert a FAT32-formatted microSD before boot |
| Save corrupt panic on restore | Power lost mid-save / FAT not flushed | Auto-wipe + reboot fires automatically; insert a fresh game from the splash |
| Pixels show but colours wrong (e.g. `@` is red) | RGB565 byte order (LE vs BE) | Already fixed via `__builtin_bswap16` in `TDECK_COLOR_RGB565` |
| Map briefly appears then disappears | Stale "winid=0 means map" assumption | Fixed: `shim_create_nhwindow` now assigns unique winids and remembers `win_map` |

## Credits

- Upstream [NetHack DevTeam](https://www.nethack.org/) — engine.
- The libnh + win/shim infrastructure follows the WebAssembly port pattern
  ([sys/libnh/README.md](sys/libnh/README.md)) substituting Xtensa for
  emscripten.
- [LilyGo](https://lilygo.cc/) — T-Deck hardware.
