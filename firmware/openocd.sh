#!/usr/bin/env bash

if [ -z "${OPENOCD:-}" ]; then
    for c in "$HOME/.local/bin/openocd" "$HOME/dev/openocd/src/openocd" openocd; do
        command -v "$c" >/dev/null 2>&1 || continue
        root="$(dirname "$(dirname "$(command -v "$c")")")"
        # An in-tree build keeps its scripts in tcl/ rather than share/.
        for sd in "$root/share/openocd/scripts" "$root/tcl"; do
            [ -f "$sd/target/rp2350.cfg" ] || continue
            OPENOCD="$c"; OOCD_SCRIPTS="$sd"; break 2
        done
        : "${OPENOCD:=$c}"
    done
fi
: "${OPENOCD:=openocd}"
: "${OOCD_SCRIPTS:=$(dirname "$(dirname "$(command -v "$OPENOCD" \
    || echo /usr/bin/openocd)")")/share/openocd/scripts}"
: "${OOCD_SPEED_KHZ:=5000}"

command -v "$OPENOCD" >/dev/null 2>&1 || {
    echo "no openocd found; install one with RP2350 support or set \$OPENOCD" >&2
    exit 1; }

sflag=(); [ -d "$OOCD_SCRIPTS" ] && sflag=(-s "$OOCD_SCRIPTS")

# Program one file at one address (empty = the ELF's own), then reset and exit.
oocd_program() {
    "$OPENOCD" "${sflag[@]}" -f interface/cmsis-dap.cfg \
        -c "transport select swd" -c "adapter speed $OOCD_SPEED_KHZ" \
        -f target/rp2350.cfg \
        -c "program \"$1\" verify reset exit ${2:-}"
}
