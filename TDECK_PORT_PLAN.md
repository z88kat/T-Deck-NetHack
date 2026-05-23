# NetHack 5.0 → LilyGo T-Deck Port Plan

Target: LilyGo T-Deck (ESP32-S3FN16R8, 16MB flash, 8MB PSRAM, 512KB SRAM, 320×240 ST7789 LCD, BlackBerry-style QWERTY keyboard, microSD slot, trackball).

Wiki: https://wiki.lilygo.cc/products/t-deck-series/t-deck/

## Strategy in one line

Build NetHack as a static library via the existing **`sys/libnh`** mode, route its rendering through the existing **`win/shim`** callback windowport, and link it into an ESP-IDF application that provides the LCD/keyboard/SD glue.

This is the same shape as the WASM build (see [Cross-compiling](Cross-compiling) section B6), substituting Xtensa cross-compiler for emscripten and a framebuffer for the browser DOM.

## Why this approach (vs. porting tty/curses directly)

- `sys/libnh/libnhmain.c` already exposes `nhmain(argc, argv)` + `shim_graphics_set_callback()` as a clean two-function library API ([sys/libnh/README.md](sys/libnh/README.md)).
- `win/shim/winshim.c` is a **single file, ~325 lines** that implements the full 96-entry windowport vtable as `(name, ret_ptr, fmt, ...)` callbacks. No termcap, no curses, no terminal escape sequences to fight.
- `Cross-compiling` doc B6 documents the libnh+shim combo as the canonical embedded pattern.
- `tty`/`curses` would drag in termcap/terminfo assumptions, escape-sequence parsing, and a ~7,000-line state machine we'd have to reverse-engineer onto a framebuffer.

## Target architecture

```
ESP-IDF app (app_main)
  ├── components/nethack/        ← built from this repo as libnethack.a
  │     └── libnethack.a         ← sys/libnh + src/* compiled with xtensa-esp32s3-elf-gcc
  ├── components/tdeck_display/  ← ST7789 driver (use LovyanGFX or LVGL)
  ├── components/tdeck_input/    ← I²C keyboard scan + trackball IRQ
  ├── components/tdeck_storage/  ← SD card via VFS, mounted at /sdcard
  └── main/
        ├── app_main.c           ← FreeRTOS task starts up NetHack
        ├── shim_callback.c      ← bridges win/shim callbacks → display/input
        └── partitions.csv       ← flash layout
```

The NetHack code is *unmodified except for*:
1. A new hints file `sys/unix/hints/esp32s3.500` for cross-compile flags.
2. Minimal `#ifdef` guards in `src/files.c` / `src/report.c` for POSIX-isms the existing UNIX ifdefs don't already cover.

## Hardware budget sanity-check

| Resource | Available | NetHack needs | Headroom |
|----------|-----------|---------------|----------|
| Flash (code+data) | 16 MB | ~2 MB code + ~3 MB nhdat/Lua + fonts | Plenty (move nhdat to SD if tight) |
| PSRAM (heap) | 8 MB | ~1–2 MB working set | Comfortable |
| SRAM (hot data) | 512 KB | ~50 KB stack + framebuffer | Tight but fine (320×240×2 = 150 KB framebuffer; partial flush or 4-bit indexed to save) |
| CPU | 240 MHz dual-core | NetHack is single-threaded, not CPU-bound | Fine |

Run NetHack on core 1, display flush on core 0.

## Display mapping (the biggest design call)

The shim windowport thinks in NetHack terms (map glyphs, status line, message line, menus). We have to decide pixel layout:

- **Map**: NetHack maps are 80×21 cells. At 320×240 we cannot fit 80 columns of readable text. Two realistic options:
  1. **Tiles**: 16×16 pixel tiles → 20×15 visible cells with scrolling. Closer to the canonical "Falcon's Eye"/Hack-like presentation, easier to read on a 2.8" screen. NetHack already supports tile rendering via the windowport; we'd ship a tileset in nhdat.
  2. **Tiny text**: 4×8 font → 80×30 cells fits 320×240 but is borderline unreadable on this screen.
- **Recommendation**: tiles. Use the existing 16×16 tileset from `win/share/`.
- **Status / message lines**: render with a separate 6×10 or 8×8 font in a strip below the map.

Decide tile size first weekend — everything else flows from it.

## Input mapping

- T-Deck keyboard is a real QWERTY → most NetHack commands map 1:1 (`h/j/k/l`, `y/u/b/n`, `i`, `Z`, `#pray`, etc.).
- Shift/Alt/Sym modifiers exist on the matrix — wire `Shift+hjkl` → run, `Alt+...` → escape sequences if we need them.
- Trackball IRQ → directional input (alternate to vi keys) or menu scroll.
- Need an "Esc" affordance — the T-Deck keyboard has no dedicated Esc; map a Sym combo.

## Storage layout

- **Internal flash partition** (LittleFS): `nhdat`, Lua scripts, default config, fonts — read-only after install. Put under `/spiffs` or `/littlefs`.
- **SD card** (FATFS at `/sdcard`): save files, bones, logfile, configurable record. NetHack writes here.
- Set `HACKDIR=/littlefs/nethack`, `SAVEDIR=/sdcard/nethack/save`, `BONESDIR=/sdcard/nethack/bones` via config.
- Enable `NOCWD_ASSUMPTIONS` so NetHack uses absolute paths and never tries to `chdir()`.

## POSIX-isms to neutralize

From the survey, all hotspots are in 2 files and already inside `#ifdef UNIX` blocks:

| File | Hazards | Plan |
|------|---------|------|
| [src/files.c](src/files.c) | `fork`, `signal`, `system`, `getuid`, `getpwuid` (~3,711 lines) | All under `#ifdef UNIX`. Build with `UNIX` undefined → they vanish. Anything that survives, stub via a new `sys/esp32s3/esp32sys.c`. |
| [src/report.c](src/report.c) | `fork`, `popen` (crash reporter) | Compile out — no crash mailer on a handheld. |
| [src/end.c](src/end.c), [src/allmain.c](src/allmain.c) | `signal()` for SIGINT/SIGHUP | Stub `signal()` as no-op in our sys layer. |

No raw `open()`/`read()` in the core game loop — file I/O is all `fopen()`, which ESP-IDF's VFS handles transparently against LittleFS and FATFS.

## Build system

NetHack 5.0 uses hand-rolled make + hints files (no CMake for the game; only the `pdcursesmod` submodule uses CMake). ESP-IDF is CMake-based. Bridge:

- **Stage 1 (host)**: build `util/makedefs`, `util/dlb`, `util/uudecode` natively on macOS (run from `sys/unix/hints/macOS.500`). These generate `pm.h`, `onames.h`, `date.h`, `nhdat` — same as the WASM build.
- **Stage 2 (target)**: invoke `make WANT_LIBNH=1 CROSS_TO_ESP32S3=1` with our new hints file. Output: `src/libnethack.a`.
- **Stage 3 (ESP-IDF)**: `idf.py build` picks up `libnethack.a` as a prebuilt component (via `add_prebuilt_library` in `components/nethack/CMakeLists.txt`).

Look at how `CROSS_TO_WASM` is plumbed through the top-level `Makefile` and copy the pattern.

## Phased milestones

### Phase 0 — Setup (an evening before the weekend)
- [ ] Install ESP-IDF v5.x and confirm `idf.py --version` works.
- [ ] Order a USB-C cable if not on hand; verify T-Deck enumerates as serial device.
- [ ] Read [Cross-compiling](Cross-compiling) section B6 (WASM/libnh case sample) end-to-end.
- [ ] Build libnethack.a natively on macOS following [sys/libnh/README.md](sys/libnh/README.md) — confirms host tools work before we cross-compile.

### Phase 1 — Cross-compile libnethack.a for Xtensa (target: half a day)
- [ ] Copy `sys/unix/hints/linux-minimal` → `sys/unix/hints/esp32s3.500` as starting point.
- [ ] Set `CC=xtensa-esp32s3-elf-gcc`, `AR=xtensa-esp32s3-elf-ar`, target flags (`-mlongcalls`), `WANT_LIBNH=1`.
- [ ] Disable network/mail/crash-report features; ensure `UNIX` is undefined.
- [ ] Iterate compile errors until `src/libnethack.a` builds clean.
- **Done when**: `file src/libnethack.a` reports Xtensa ELF.

### Phase 2 — Minimal ESP-IDF skeleton (target: half a day)
- [ ] `idf.py create-project tdeck-nethack` in a sibling directory (not in this repo).
- [ ] Add `components/nethack/` that consumes `libnethack.a` as `IMPORTED` static library.
- [ ] In `app_main`: register a stub shim callback that just `ESP_LOGI`s `name` for every call, then `nhmain(0, NULL)`.
- [ ] Flash & monitor — see what window calls NetHack emits during startup. This is our spec.
- **Done when**: serial console shows the sequence of windowport calls NetHack makes before it would draw the title screen.

### Phase 3 — LCD output (target: a day)
- [ ] Wire up ST7789 via `esp_lcd` driver. Pin map from T-Deck schematic (CS=12, DC=11, SCK=40, MOSI=41, RST=GPIO0 via IO expander).
- [ ] Decide tile vs. text — recommend 16×16 tiles for the map (20×15 visible) + 8×8 text for status/menus.
- [ ] Implement enough shim callbacks to render the title screen and the map: `init_nhwindows`, `create_nhwindow`, `clear_nhwindow`, `print_glyph`, `putstr`, `display_nhwindow`, `curs`.
- [ ] Ship a tileset in nhdat (existing `win/share/` tiles) or generate from font for v0.
- **Done when**: title screen renders on LCD.

### Phase 4 — Keyboard input (target: half a day)
- [ ] T-Deck keyboard is a TCA8418 (or similar) I²C scanner — driver exists in Arduino/ESP-IDF examples.
- [ ] Implement shim `nhgetch`, `nh_poskey`, `yn_function` against a FreeRTOS queue fed by the keyboard ISR.
- [ ] Map shift/alt/sym + base key → ASCII the way NetHack expects.
- **Done when**: can walk a character around the dungeon.

### Phase 5 — Persistence (target: half a day)
- [ ] Mount SD card via ESP-IDF SDMMC at `/sdcard`.
- [ ] Mount LittleFS partition holding nhdat at `/littlefs`.
- [ ] Set `HACKDIR`/`SAVEDIR`/`BONESDIR` in libnh sysconf.
- [ ] Confirm save/restore round-trips across reboots.
- **Done when**: can save a game, power-cycle, resume.

### Phase 6 — Polish
- [ ] Menus, inventory rendering, multi-line prompts.
- [ ] Backlight/power management; sleep on inactivity.
- [ ] Trackball as alternate input.
- [ ] Audio? T-Deck has a speaker — NetHack 5.0 has optional sound hooks ([sound/](sound/) directory).

## Open questions to decide at the weekend

1. **Tiles or text-on-LCD?** (Phase 3 hinges on this.) Strongly leaning tiles.
2. **LVGL vs. raw `esp_lcd`?** LVGL gives us a widget toolkit for menus but adds ~200 KB. Raw `esp_lcd` is leaner. Suspect raw + a small text/blit helper is right.
3. **Lua memory footprint?** NetHack 5.0 mandates Lua ([lib/lua-5.4.8/](lib/lua-5.4.8/)). Need to confirm it links cleanly for Xtensa and doesn't blow the heap. ESP-IDF has its own Lua component we can compare against.
4. **Where does nhdat live?** Internal LittleFS (faster, wears flash less than SD) or SD (easier to update). Probably LittleFS, ~3 MB partition.
5. **Do we keep this fork upstream-clean?** If yes, the only in-tree change is the new hints file + minimal `#ifdef` guards. All ESP-IDF glue lives in a separate `tdeck-nethack` repo that consumes this one as a submodule.

## Reference checklist of files we will touch in this repo

- **New**: [sys/unix/hints/esp32s3.500](sys/unix/hints/esp32s3.500) — cross-compile hints file.
- **New (maybe)**: [sys/esp32s3/](sys/esp32s3/) — stubs for any POSIX calls not already `#ifdef UNIX`-guarded.
- **Modify (minimal)**: [src/files.c](src/files.c), [src/report.c](src/report.c) — only if existing ifdefs don't cover us.
- **Modify**: top-level [Makefile](Makefile) — add `CROSS_TO_ESP32S3` plumbing parallel to `CROSS_TO_WASM`.

Everything else (ST7789 driver, keyboard scan, SD glue, shim callback bridge) lives in a separate ESP-IDF project that consumes the libnethack.a artifact.

## Prior art to look at before starting

- [sys/libnh/README.md](sys/libnh/README.md) — the API contract.
- [win/shim/winshim.c](win/shim/winshim.c) — the callback windowport implementation.
- [Cross-compiling](Cross-compiling) sections B4 (msdos) and B6 (WASM/libnh).
- [doc/window.txt](doc/window.txt) — windowport interface spec (referenced by libnh README).
- Existing ESP32 LCD roguelike ports (community projects on GitHub) for hardware bring-up hints — don't copy the gameplay code, just the driver patterns.

## Quick-start command for weekend

```bash
# Phase 0 — host build sanity check
cd sys/unix
./setup.sh hints/macOS.500
cd ../..
make WANT_LIBNH=1 all
# Should produce src/libnethack.a as a Mach-O ARM64/x86_64 static lib
file src/libnethack.a
```

If that works, Phase 1 is just swapping the hints file for our Xtensa one and re-running.
