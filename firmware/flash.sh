#!/usr/bin/env bash
# Build + flash the firmware onto the PGA2350 over SWD
# Usage:  ./flash.sh [--no-build] [--monitor]
# Env:    OPENOCD, OOCD_SPEED_KHZ (5000), PICO_SDK_PATH.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Only export a path that exists: an unset PICO_SDK_PATH lets cmake fall back
# to PICO_SDK_FETCH_FROM_GIT, and a bogus one would break that.
if [ -z "${PICO_SDK_PATH:-}" ] && [ -d "$HOME/dev/pico-sdk" ]; then
    PICO_SDK_PATH="$HOME/dev/pico-sdk"
fi
[ -n "${PICO_SDK_PATH:-}" ] && export PICO_SDK_PATH

. "$here/openocd.sh"
build="$here/build"; elf="$build/nescd.elf"
JOBS="$(nproc 2>/dev/null || echo 4)"

do_build=1 monitor=0
for a in "$@"; do case "$a" in
    --no-build) do_build=0 ;;
    --monitor)  monitor=1 ;;
    *) echo "unknown option: $a" >&2; exit 1 ;;
esac; done

if [ "$do_build" = 1 ]; then
    cmake -S "$here" -B "$build" >/dev/null
    cmake --build "$build" --target nescd -j "$JOBS"
fi
[ -f "$elf" ] || { echo "missing ELF: $elf (build first)"; exit 1; }

echo ">> flashing over SWD: $(basename "$elf")  (openocd: $OPENOCD)"
oocd_program "$elf"

if [ "$monitor" = 1 ]; then
    dev=/dev/ttyACM1
    for d in /dev/ttyACM*; do
        [ "$(cat /sys/class/tty/$(basename "$d")/device/bInterfaceNumber 2>/dev/null)" = "01" ] && { dev="$d"; break; }
    done
    echo ">> monitor: picocom $dev 115200 (Ctrl-A Ctrl-X to exit)"
    exec picocom -qb 115200 "$dev"
fi
