#!/usr/bin/env python3
"""Check a packed .n8p the way `src/n8push.c` reads it.

    test_n8push.py IMAGE.n8p [LOCAL=SD ...]

mkn8push's own --verify checks the bytes it just wrote from the objects still in
memory.  This re-reads the packed image cold and re-derives every field from the
bytes alone, using the same rules the firmware applies, then re-checks each
payload against the local file it came from if the pairs are given again.

The rule worth the test is `off + len <= payload_bytes` per entry.  A file that
runs past the payload is the one malformation the firmware cannot notice at
push time: it would happily stream flash past the end of the staging window
into whatever the next erase block holds and write that onto the SD card.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkn8push import (FILE_FMT, HDR_FMT, HDR_SIZE, LIMIT,  # noqa: E402
                      MAGIC, MAXFILE, PATH_LEN, VERSION)

fails = 0


def check(cond, what):
    global fails
    print(f"  {what:<52} {'ok' if cond else 'FAIL'}")
    if not cond:
        fails += 1


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    pairs = [p.split("=", 1) for p in sys.argv[2:]]

    with open(path, "rb") as fh:
        img = fh.read()

    print(f"{path}: {len(img)} B")
    check(len(img) >= HDR_SIZE, f"image holds a {HDR_SIZE} B header")
    check(len(img) <= LIMIT, f"image fits the {LIMIT >> 20} MiB staging window")

    magic, version, nfiles, poff, pbytes, crc, boot = struct.unpack_from(HDR_FMT, img, 0)
    check(magic == MAGIC, f"magic {magic!r}")
    check(version == VERSION, f"version {version}")
    check(1 <= nfiles <= MAXFILE, f"nfiles {nfiles} in 1..{MAXFILE}")
    check(poff >= HDR_SIZE, f"payload_off {poff} clears the header")
    check(poff + pbytes <= len(img), f"payload {poff}+{pbytes} inside the image")

    payload = img[poff:poff + pbytes]
    check(zlib.crc32(payload) & 0xFFFFFFFF == crc, f"payload crc {crc:08X}")

    boot = boot.split(b"\0")[0].decode()
    check(not boot or not boot.startswith("/"), f"boot {boot or '(none)'} is relative")

    base = struct.calcsize(HDR_FMT)
    step = struct.calcsize(FILE_FMT)
    seen = []
    for i in range(nfiles):
        off, length, raw = struct.unpack_from(FILE_FMT, img, base + i * step)
        name = raw.split(b"\0")[0].decode()
        check(b"\0" in raw, f"file {i} path terminated inside {PATH_LEN} B")
        check(name and not name.startswith("/"), f"file {i} {name!r} is relative")
        check(length > 0, f"file {i} is non-empty")
        check(off + length <= pbytes, f"file {i} {off}+{length} inside the payload")
        seen.append((name, payload[off:off + length]))

    unused = img[base + nfiles * step:HDR_SIZE]
    check(unused == bytes(len(unused)), "unused file entries are zeroed")

    if boot:
        check(boot in [n for n, _ in seen] or not pairs,
              f"boot target {boot} is one of the pushed files")

    for (local, sd), (name, blob) in zip(pairs, seen):
        with open(local, "rb") as fh:
            want = fh.read()
        check(name == sd, f"{os.path.basename(local)} -> {name}")
        check(blob == want, f"{name} is byte-exact ({len(blob)} B)")

    print(f"\n{'FAILED' if fails else 'all checks passed'}"
          f"{f' ({fails})' if fails else ''}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
