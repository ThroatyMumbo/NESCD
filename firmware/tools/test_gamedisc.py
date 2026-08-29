#!/usr/bin/env python3
"""Check a NESCDISC image cold, the way `src/catalog.c` will: the catalog's
own rules, then every item by its own container's test.

    test_gamedisc.py game.img

Every item is crc'd over its padded sectors, the ROM is unpacked with
mkn8push's reader, tracks run through test_track, and audiofmt.image_blocks()
must find the same blocks.  A reserved item's shape is checked, its content is
not, since this firmware refuses to serve it.
"""
import argparse
import mmap
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkn8push                                              # noqa: E402
import test_track                                            # noqa: E402
from audiofmt import image_blocks                            # noqa: E402
from mkgamedisc import (FIRST_LBA, HDR_LEN, ITEM_LEN,        # noqa: E402
                        ITEM_NAME, ITEM_RESERVED, ITEM_ROM, ITEM_TRACK,
                        MAXITEMS, SECTOR, TRACK_MAX, read_catalog)

failed = 0


def check(cond, what):
    global failed
    print(f"  {'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        failed += 1


def check_image(img):
    """Returns the number of failed checks."""
    global failed
    failed = 0
    check(len(img) % SECTOR == 0, "image is a whole number of sectors")
    total = len(img) // SECTOR
    try:
        hdr, items = read_catalog(img)
    except ValueError as e:
        check(False, f"catalog: {e}")
        return failed
    check(True, f"catalog: {hdr['title']!r}, {hdr['nitems']} items")
    check(hdr["reserved"] == (0, 0, 0, 0), "header reserved words are zero")
    check(not any(img[HDR_LEN + hdr["nitems"] * ITEM_LEN:SECTOR]),
          "unused table entries and the rest of sector 0 are zero")
    check(not any(img[SECTOR:FIRST_LBA * SECTOR]),
          f"sectors 1..{FIRST_LBA - 1} are zero")

    roms = [i for i in items if i["type"] == ITEM_ROM]
    check(len(roms) == 1, "exactly one ROM item")
    ids = [i["id"] for i in items if i["type"] == ITEM_TRACK]
    check(len(ids) == len(set(ids)), "track ids are unique")
    check(all(1 <= i <= TRACK_MAX for i in ids), f"track ids within 1..{TRACK_MAX}")
    check(all(i["type"] in ITEM_NAME for i in items), "every item type known")
    check(all(i["reserved"] == (0, 0, 0) for i in items), "item reserved zero")
    check(all(i["lba"] >= FIRST_LBA for i in items), f"items start at LBA {FIRST_LBA}")
    check(all(i["sectors"] > 0 for i in items), "no empty item")
    ends = [(i["lba"], i["lba"] + i["sectors"]) for i in items]
    check(all(b[0] >= a[1] for a, b in zip(ends, ends[1:])),
          "items ascending and non-overlapping")
    check(all(e <= total for _, e in ends), "every item inside the image")
    check(ends and ends[-1][1] == total, "image ends with its last item")
    if failed:
        return failed

    for it in items:
        at, n = it["lba"] * SECTOR, it["sectors"] * SECTOR
        view = img[at:at + n]
        name = f"{ITEM_NAME[it['type']]} {it['id']}"
        check(zlib.crc32(view) == it["crc32"], f"{name}: crc over {it['sectors']} sectors")
        if it["type"] == ITEM_ROM:
            try:
                h, files = mkn8push.unpack(view)
                names = dict(files)
                check(bool(h["boot"]), f"{name}: names a boot target")
                check(h["boot"] in names, f"{name}: boots one of its own files")
                nes = names.get(h["boot"], b"")
                mapper = (nes[7] & 0xF0) | (nes[6] >> 4) if len(nes) >= 16 else -1
                check(mapper >= 0, f"{name}: ROM is mapper {mapper}")
                end = h["payload_off"] + h["payload_bytes"]
                check(not any(view[end:]), f"{name}: tail past the payload is zero")
            except ValueError as e:
                check(False, f"{name}: {e}")
        elif it["type"] == ITEM_RESERVED:
            check(True, f"{name}: reserved, not served by this firmware")
        elif it["type"] == ITEM_TRACK:
            try:
                aoff, abytes = struct.unpack_from("<II", view, 8 + 16 * 4)
                end = aoff + abytes
                check(end <= n, f"{name}: blocks fit the item")
                t = test_track.check_track(bytes(view[:end]))
                check(not any(view[end:]), f"{name}: tail past the blocks is zero")
                fmt, spr, blk, blocks = image_blocks(img, it["id"])
                check(len(blocks) == t["nblocks"]
                      and bytes(blocks[0]) == bytes(view[t["aoff"]:t["aoff"] + blk]),
                      f"{name}: image_blocks finds the same {t['nblocks']} blocks")
            except AssertionError as e:
                check(False, f"{name}: {e}")
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    args = ap.parse_args()

    with open(args.image, "rb") as fh:
        img = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        n = check_image(img)
        if not n:
            _hdr, items = read_catalog(img)
            print(f"  {'type':<6}{'id':>3}{'lba':>8}{'sectors':>9}  crc32")
            for it in items:
                print(f"  {ITEM_NAME[it['type']]:<6}{it['id']:>3}{it['lba']:>8}"
                      f"{it['sectors']:>9}  {it['crc32']:08X}")
        img.close()
    print("all checks passed" if not n else f"FAILED ({n})")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
