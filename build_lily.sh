#!/usr/bin/env bash
# build_lily.sh -- one-shot build + flash + monitor for the LilyGo T-Deck.
#
# Stages (run conditionally; pass --help for flags):
#   1. ensure ESP-IDF env is sourced
#   2. host build of libnh (if missing or src changed)
#   3. Xtensa cross-compile of libnh.a + hacklib.a + lua548.a
#   4. ESP-IDF firmware build via idf.py
#   5. flash via the first /dev/cu.usbmodem* that's present
#   6. open serial monitor (Ctrl+] to quit)
#
# Tested on macOS with ESP-IDF v5.3.2.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LILYGO="$REPO/lilygo"
IDF_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"

# --- flags -------------------------------------------------------------
DO_HOST=1
DO_CROSS=1
DO_FW=1
DO_FLASH=1
DO_MONITOR=1
PORT=""

usage() {
    cat <<EOF
build_lily.sh -- build + flash + monitor for the LilyGo T-Deck

Usage: $0 [options]

  --no-host       Skip the host \`make WANT_LIBNH=1 all\` stage
  --no-cross      Skip the Xtensa \`make esp32s3\` stage
  --no-fw         Skip the ESP-IDF firmware build
  --no-flash      Don't flash to the device (build only)
  --no-monitor    Don't open the serial monitor after flashing
  --port PATH     Serial port (default: first /dev/cu.usbmodem* found)
  --clean         \`idf.py fullclean\` before building (forces full rebuild)
  -h, --help      Show this help

Typical iteration: edit code, then run \`./build_lily.sh\`.  The script
short-circuits stages whose outputs are already current.
EOF
}

DO_CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-host)    DO_HOST=0   ;;
        --no-cross)   DO_CROSS=0  ;;
        --no-fw)      DO_FW=0     ;;
        --no-flash)   DO_FLASH=0  ;;
        --no-monitor) DO_MONITOR=0 ;;
        --port)       PORT="$2"; shift ;;
        --clean)      DO_CLEAN=1  ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "unknown flag: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

# --- helpers -----------------------------------------------------------
log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mxxx\033[0m %s\n' "$*" >&2; exit 1; }

# --- 1. ESP-IDF env ----------------------------------------------------
if [[ -z "${IDF_PATH:-}" ]]; then
    [[ -f "$IDF_EXPORT" ]] || die "ESP-IDF not found at $IDF_EXPORT -- set IDF_PATH or install per README-lilly.md"
    log "sourcing ESP-IDF env from $IDF_EXPORT"
    # shellcheck disable=SC1090
    source "$IDF_EXPORT" >/dev/null
fi

command -v idf.py >/dev/null || die "idf.py not on PATH after sourcing -- ESP-IDF install incomplete?"

# --- 2. host build -----------------------------------------------------
if (( DO_HOST )); then
    if [[ ! -f "$REPO/dat/nhdat" || ! -f "$REPO/src/libnh.a" ]]; then
        log "host build (make WANT_LIBNH=1 all)"
        if [[ ! -d "$REPO/lib/lua-5.4.8" ]]; then
            log "fetching Lua (first-time setup)"
            (cd "$REPO" && make fetch-lua)
        fi
        if [[ ! -f "$REPO/src/Makefile" ]]; then
            log "running sys/unix setup.sh"
            (cd "$REPO/sys/unix" && ./setup.sh hints/macOS.500)
        fi
        (cd "$REPO" && make WANT_LIBNH=1 all)
    else
        log "host build artifacts present; skipping (--no-host to disable, or rm dat/nhdat to force)"
    fi
fi

# --- 3. Xtensa cross-compile ------------------------------------------
if (( DO_CROSS )); then
    log "Xtensa cross-compile (make esp32s3)"
    # Avoid the host-CC-with-target-CFLAGS regression in the util/ sub-make:
    # touching tile.c keeps it newer than config.h so make doesn't regen it.
    touch "$REPO/src/tile.c"
    (cd "$REPO" && make esp32s3)
fi

# --- 4. firmware build -------------------------------------------------
if (( DO_FW )); then
    if (( DO_CLEAN )); then
        log "idf.py fullclean"
        (cd "$LILYGO" && idf.py fullclean)
    fi
    log "ESP-IDF firmware build (idf.py build)"
    (cd "$LILYGO" && idf.py build)
fi

# --- 5. port autodetect + flash ---------------------------------------
if (( DO_FLASH )); then
    if [[ -z "$PORT" ]]; then
        # Most LilyGo T-Decks enumerate as /dev/cu.usbmodemXXXX on macOS.
        # Use compgen so a no-match expands to an empty list, not the glob.
        PORTS=( $(compgen -G "/dev/cu.usbmodem*" || true) )
        if (( ${#PORTS[@]} == 0 )); then
            die "no /dev/cu.usbmodem* found -- plug the T-Deck in or pass --port PATH"
        fi
        PORT="${PORTS[0]}"
        if (( ${#PORTS[@]} > 1 )); then
            warn "multiple ports found: ${PORTS[*]}  -- using $PORT"
        fi
    fi
    log "flashing to $PORT"
    (cd "$LILYGO" && idf.py -p "$PORT" flash)
fi

# --- 6. monitor --------------------------------------------------------
if (( DO_MONITOR )); then
    [[ -n "$PORT" ]] || PORT="$(compgen -G "/dev/cu.usbmodem*" | head -1 || true)"
    [[ -n "$PORT" ]] || die "no port for monitor -- pass --port PATH or --no-monitor"
    log "opening serial monitor on $PORT  (Ctrl+] to quit)"
    (cd "$LILYGO" && idf.py -p "$PORT" monitor)
fi

log "done"
