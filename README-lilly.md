# NetHack 5.0 on the LilyGo T-Deck

A port of [NetHack 5.0](https://github.com/NetHack/NetHack) to the
[LilyGo T-Deck](https://lilygo.cc/products/t-deck/) handheld — a 320x240
colour TFT, BlackBerry-style QWERTY keyboard, microSD slot, and trackball
on an ESP32-S3 with 16 MB flash and 8 MB PSRAM.

The full upstream NetHack engine runs as a static library
(`libnh.a`) on Xtensa; an ESP-IDF application in `lilygo/` provides the
display, keyboard, and filesystem glue.  See
[TDECK_PORT_PLAN.md](TDECK_PORT_PLAN.md) for the original design.

## Status

| Phase | Description | State |
|------:|-------------|------|
| 0 | Host build of `libnh.a` on macOS | ✅ |
| 1 | Cross-compile `libnh.a` + `hacklib.a` + `lua548.a` for Xtensa | ✅ |
| 2 | ESP-IDF skeleton: shim windowport, FreeRTOS task, log every windowport call | ✅ |
| 3 | ST7789 display: 8x8 ASCII glyph rendering, 40-col viewport scrolling around player | ✅ |
| 4 | I²C QWERTY keyboard: real input through `shim_nhgetch` / `shim_yn_function` | ✅ |
| 5 | SD card mount, save/restore round-trip | ⏳ |
| 6 | Polish: trackball, sleep / backlight management, sound, status colour | ⏳ |

You can boot, name your character (default name = `player`), see the dungeon,
walk around with `hjkl`, fight monsters, pick up items, and play the game
end-to-end.  Saves currently don't persist because the SD card isn't mounted
yet (Phase 5).

## Hardware requirements

- **LilyGo T-Deck** (ESP32-S3FN16R8 variant: 16 MB flash, 8 MB OPI PSRAM).
- USB-C cable for flashing and serial console.
- Optional but recommended for Phase 5+: a microSD card (FAT32-formatted)
  for save files.

## Software requirements

### Host (macOS)

- Xcode Command Line Tools (`xcode-select --install`) — provides clang, make.
- Homebrew packages: `cmake`, `ninja`, `dfu-util`.
  ```bash
  brew install cmake ninja dfu-util
  ```
- Python 3.8+ (system Python on macOS 12+ works).
- Apple `libtool` (ships with Xcode CLT) — used for the host `libnh.a`.

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
found and is required by the application: ruamel.yaml.clib", patch the script
to PEP-503-normalise lookups:

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
│  │   main/app_main.c            -- mount FS, start task     │    │
│  │   main/shim_callback.c       -- windowport bridge        │    │
│  │   components/tdeck_display   -- ST7789 driver            │    │
│  │   components/tdeck_keyboard  -- I²C QWERTY driver        │    │
│  └──────────────────┬───────────────────────────────────────┘    │
└─────────────────────┼──────────────────────────────────────────  ┘
                      │   idf.py flash
                      ▼
              ┌────────────────────┐
              │  LilyGo T-Deck     │
              │  ESP32-S3 + LCD    │
              │  + QWERTY          │
              └────────────────────┘
```

## Build

Three stages, in order.  The first two only need to run when the NetHack
engine source changes; the third is your edit-loop iteration.

### 1. Host build (one-time per engine change)

Produces `util/makedefs`, `util/dlb`, `dat/nhdat`, and a host `src/libnh.a`
to validate the engine compiles.

```bash
cd sys/unix && ./setup.sh hints/macOS.500 && cd ../..
make WANT_LIBNH=1 all
```

If this is your first ever build, fetch Lua first:

```bash
make fetch-lua
```

### 2. Xtensa cross-compile (one-time per engine change)

Produces `targets/esp32s3/{libnh,hacklib,lua548}.a` — the Xtensa-ELF static
archives the ESP-IDF project consumes.

```bash
. ~/esp/esp-idf/export.sh
make esp32s3
```

### 3. ESP-IDF firmware build (every iteration)

```bash
cd lilygo
idf.py build
```

The build:

- imports `../targets/esp32s3/*.a` via `components/nethack/`
- routes libnh's large `.bss` (mons[], objects[], gg, svl, glyphmap, ...) into
  PSRAM via `components/nethack/linker.lf` and the `NH_EXTRAM` section
  attribute (see `include/config.h`)
- builds a SPIFFS image from `../dat/nhdat` (auto-staged via `configure_file`)
  and flashes it to the `nhdat` partition at `0x410000`

Total firmware size ≈ 2.3 MB, plus the 3 MB nhdat partition.

## Flash + run

```bash
cd lilygo
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem* flash monitor
```

(Adjust the port for your USB enumeration; on most macOS hosts the T-Deck
appears as `/dev/cu.usbmodem21101` or similar.)

In the serial monitor, **press `Ctrl+]`** to quit (Ctrl+C does not work).

### What to expect

1. ESP-IDF boot log → PSRAM init → heap init → `Calling app_main()`.
2. LCD test pattern: RED → GREEN → BLUE → WHITE → "T-Deck NetHack 5.0" splash.
3. `nhdat mounted at /nethack` log line.
4. NetHack starts; the shim logs each windowport call to the serial console
   (e.g. `nh-shim: shim_init_nhwindows ret=v (...)`).
5. The dungeon appears on the LCD with the player `@` highlighted in green.
   Intro text shows below the map.
6. Press a key (e.g. **space**) to dismiss the intro.
7. `hjkl` to move, `y/u/b/n` for diagonals, `i` for inventory, `,` to pick up,
   `s` to search, `` ` `` (backtick) for ESC.

## Hardware pin map

For the standard LilyGo T-Deck.  Override the `#define`s in the relevant
component header if your variant differs.

| Function | Pin | Source file |
|----------|-----|-------------|
| Peripheral power enable (LCD/keyboard/SD) | GPIO 10 | `lilygo/components/tdeck_display/tdeck_display.c` |
| LCD: SPI SCLK | GPIO 40 | `tdeck_display.h` |
| LCD: SPI MOSI | GPIO 41 | `tdeck_display.h` |
| LCD: CS       | GPIO 12 | `tdeck_display.h` |
| LCD: DC       | GPIO 11 | `tdeck_display.h` |
| LCD: backlight | GPIO 42 (active high) | `tdeck_display.h` |
| Keyboard: I²C SDA | GPIO 18 | `tdeck_keyboard.h` |
| Keyboard: I²C SCL | GPIO 8  | `tdeck_keyboard.h` |
| Keyboard: INT | GPIO 46 (polled, not yet wired as IRQ) | `tdeck_keyboard.h` |
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

RAM:

- `.dram0.bss` ≈ 2.4 KB (ESP-IDF system globals only)
- `.dram0.data` ≈ 34 KB
- `.iram0.text` ≈ 54 KB
- `.ext_ram.bss` ≈ 407 KB (NetHack engine globals — routed to PSRAM)
- `.flash.text` ≈ 1.6 MB
- `.flash.rodata` ≈ 521 KB

Free PSRAM heap available at runtime ≈ 7.7 MB.

## Project structure

In-tree additions for the ESP32-S3 port:

```
T-Deck-NetHack/
├── sys/esp32s3/esp32sys.c                 # POSIX shim stubs (fork, getpwuid, …)
├── sys/libnh/libnhmain.c                  # added nh_glyph_info_char(),
│                                          #   fqn_prefix init for ESP32
├── sys/unix/hints/esp32s3.500             # cross-compile hints
├── sys/unix/hints/include/
│   ├── cross-pre1.500                     # CROSS_TO_ESP32S3 PRE-1 block
│   ├── cross-pre2.500                     # main CFLAGS / SYSSRC / SYSOBJ block
│   └── cross-post.500                     # archive rule
├── include/config.h                       # NH_EXTRAM macro + NOSYSCF guard
├── include/decl.h                         # …
├── src/{decl,display,monst,objects}.c     # NH_EXTRAM on big globals
└── lilygo/                                # ESP-IDF firmware project
    ├── CMakeLists.txt                     # stages nhdat from ../dat/
    ├── partitions.csv
    ├── sdkconfig.defaults                 # PSRAM, BSS-in-PSRAM, freertos 1 ms
    ├── main/
    │   ├── app_main.c                     # boot, mount, kick off nethack task
    │   ├── shim_callback.c                # windowport bridge → LCD + keyboard
    │   └── CMakeLists.txt
    └── components/
        ├── nethack/                       # IMPORTS Xtensa .a files
        │   ├── CMakeLists.txt
        │   ├── linker.lf                  # routes libnh's .ext_ram.bss to PSRAM
        │   └── include/nethack.h          # public API for the shim
        ├── tdeck_display/                 # ST7789 driver (esp_lcd)
        │   ├── CMakeLists.txt
        │   ├── tdeck_display.c
        │   └── include/tdeck_display.h
        └── tdeck_keyboard/                # I²C QWERTY driver
            ├── CMakeLists.txt
            ├── tdeck_keyboard.c
            └── include/tdeck_keyboard.h
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `idf.py` complains `Package was not found and is required: ruamel.yaml.clib` | ESP-IDF v5.3.2 + stdlib `importlib.metadata` PEP 503 bug | Patch `check_python_dependencies.py` (see Software requirements above) |
| `Cache disabled but cached memory region accessed` panic in `decl_globals_init` | NetHack's `.bss` lands inside `.ext_ram.dummy` reservation | Verify `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` is set in `lilygo/sdkconfig` |
| `nhdat mount failed: ESP_ERR_INVALID_ARG (0x102)` | VFS path > 15 chars | HACKDIR / mount path must be ≤ 15 chars (currently `/nethack`) |
| `nhl_loadlua: Error opening (nhlib.lua)` | nhdat partition not flashed, or `fqn_prefix[DATAPREFIX]` was NULL | Re-flash; `make WANT_LIBNH=1 all` to rebuild `dat/nhdat` |
| LCD backlight on but screen all black | Peripheral power not enabled | Confirm `GPIO 10` is set HIGH (handled by `tdeck_display_init`) |
| Pixels show but colours wrong (e.g. `@` is red) | RGB565 byte order (LE vs BE) | Already fixed in `TDECK_COLOR_RGB565` macro via `__builtin_bswap16` |
| Map briefly appears then disappears | Stale "winid=0 means map" assumption | Fixed: `shim_create_nhwindow` now assigns unique winids and remembers `win_map` |
| Watchdog reset during steady-state moveloop | NetHack input loops with no yield | Fixed: `vTaskDelay(1)` in `nh_shim_callback` |

## Known issues

- **Status text clips the first character on the left** when wrapping the
  welcome line.  Cosmetic; one-cell offset in `draw_status_message`.
- **Intro text flashes by too fast to read.**  NetHack's `display_nhwindow`
  expects a blocking "press any key" for multi-page text; not yet wired.
  Read it in the serial monitor's `nh-shim: shim_putstr s="..."` lines.
- **No save/restore** — Phase 5 lands the SD card mount.
- **Menus other than yes/no are unreachable** — `shim_select_menu` returns
  -1 (cancel) until proper menu navigation is implemented.
- **No colour on the map** — every cell is white-on-black.  NetHack supplies
  colour info via `glyph_info.framecolor` and `glyph_map.classic_representation.color`,
  not yet plumbed through the shim.

## Next steps

- **Phase 5**: mount SD card at `/sdcard` via `esp_vfs_fat_sdmmc_mount`; set
  `SAVEPREFIX`/`BONESPREFIX`/`LOCKPREFIX` to point into the SD.  Verify save
  → power-cycle → restore round-trip.
- **Phase 6**: trackball (alternate diagonals), backlight dim on inactivity,
  sound via the speaker, message-window scroll, full menu navigation.

## Credits

- Upstream [NetHack DevTeam](https://www.nethack.org/) — engine.
- The libnh + win/shim infrastructure follows the WebAssembly port pattern
  ([sys/libnh/README.md](sys/libnh/README.md)) substituting Xtensa for
  emscripten.
- [LilyGo](https://lilygo.cc/) — T-Deck hardware.
