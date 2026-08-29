#!/usr/bin/env bash
# Burn a game disc image to a CD-R and read it back to prove it.
#
# Usage:  ./burn.sh game.img [--dev /dev/sr0] [--speed 8] [--no-verify]
# Env:    CDR_DEV, CDR_SPEED.
set -euo pipefail

img="" dev="${CDR_DEV:-}" speed="${CDR_SPEED:-8}" verify=1
while [ $# -gt 0 ]; do
    case "$1" in
        --dev)        dev="$2"; shift ;;
        --speed)      speed="$2"; shift ;;
        --no-verify)  verify=0 ;;
        -*) echo "unknown option: $1" >&2; exit 1 ;;
        *)  img="$1" ;;
    esac
    shift
done

[ -n "$img" ] || { echo "usage: ./burn.sh game.img [--dev /dev/sr0]" >&2; exit 1; }
[ -f "$img" ] || { echo "no such image: $img" >&2; exit 1; }
command -v xorriso >/dev/null || { echo "xorriso is not installed" >&2; exit 1; }

[ "$(head -c 8 "$img")" = "NESCDISC" ] ||
    echo ">> warning: $img has no NESCDISC catalog at sector 0"

size=$(stat -c %s "$img")
[ $((size % 2048)) -eq 0 ] || { echo "$img is not a whole number of sectors" >&2; exit 1; }
sectors=$((size / 2048))

if [ -z "$dev" ]; then
    dev="$(xorriso -devices 2>/dev/null | grep -oE '/dev/sr[0-9]+' | head -1 || true)"
fi
[ -n "$dev" ] || { echo "no burner found; pass --dev or set CDR_DEV" >&2; exit 1; }

echo ">> burning $img ($sectors sectors, $((size / 1048576)) MiB) to $dev at ${speed}x"
xorriso -as cdrecord -v dev="$dev" speed="$speed" -sao -pad "$img"

[ "$verify" = 1 ] || exit 0

# Read back through xorriso's own SCSI path, not the block device: the kernel
# keeps the capacity it cached while the disc was blank, so /dev/srN reads as
# one sector straight after a burn and `dd | cmp` reports a bogus early EOF.
# Revalidating needs root or a tray cycle; xorriso ignores the block device.
# The "No ISO 9660 image at LBA 0" notice is expected -- this is a raw
# container, not a filesystem.
echo ">> verifying $sectors sectors"
if xorriso -indev "$dev" -check_media data_to=- use=indev what=disc \
           min_lba=0 max_lba=$((sectors - 1)) -- 2>/dev/null | cmp - "$img"; then
    echo ">> verified byte-exact"
else
    echo ">> VERIFY FAILED" >&2
    exit 1
fi
