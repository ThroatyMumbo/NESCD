#!/usr/bin/env python3
"""Pack files into an N8PUSH1 image the PGA2350 pushes onto the cart's SD card.

    mkn8push.py game.nes=nescd/game.nes \
                --boot nescd/game.nes -o out.n8p

`mkgamedisc.py` puts the image on a disc as its ROM item, where `src/n8push.c`
stages it in PSRAM and pushes it over the cart link.

Every file and the install go in one pass: the cart leaves its ROM-select menu
the moment a game boots, and there is no working host-side reset.

Layout, all little-endian, offsets in bytes:

    0     100    header
    100   576    8 file entries of u32 off, u32 len, char path[64]
    1024  ...    payload, entries back to back in order

`off` is from the payload base, not the image base, which is what lets the
header grow without moving anything.
"""
import argparse
import os
import struct
import sys
import zlib

MAGIC    = b"N8PUSH1\0"
VERSION  = 1
MAXFILE  = 8
PATH_LEN = 64
HDR_FMT  = "<8s5I8x64s"           # magic, 5 u32, reserved[2], boot
FILE_FMT = "<II64s"
HDR_SIZE = struct.calcsize(HDR_FMT) + MAXFILE * struct.calcsize(FILE_FMT)
PAYLOAD_OFF = 1024                # 4-aligned, clear of the header, room to grow

# Must match N8PUSH_LIMIT in src/n8push.h.
LIMIT = 3 << 20

assert HDR_SIZE == 676, HDR_SIZE


def check_path(p):
    """The cart's FS rules, enforced here so the firmware never has to guess."""
    if not p or p.startswith("/"):
        raise SystemExit(f"destination must be a relative path: {p!r}")
    if len(p.encode()) >= PATH_LEN:
        raise SystemExit(f"destination is over {PATH_LEN - 1} bytes: {p!r}")
    if "\\" in p:
        raise SystemExit(f"use '/' as the separator: {p!r}")
    return p


def pack(entries, boot=""):
    """entries: [(local_path, sd_path)].  Returns the image bytes."""
    if not entries:
        raise SystemExit("nothing to push")
    if len(entries) > MAXFILE:
        raise SystemExit(f"at most {MAXFILE} files per image, got {len(entries)}")
    if boot:
        check_path(boot)

    payload = bytearray()
    table = []
    for local, sd in entries:
        check_path(sd)
        with open(local, "rb") as fh:
            blob = fh.read()
        if not blob:
            raise SystemExit(f"{local} is empty")
        table.append((len(payload), len(blob), sd))
        payload += blob

    total = PAYLOAD_OFF + len(payload)
    if total > LIMIT:
        raise SystemExit(f"{total} B exceeds the {LIMIT} B staging window")

    head = struct.pack(HDR_FMT, MAGIC, VERSION, len(table), PAYLOAD_OFF,
                       len(payload), zlib.crc32(payload) & 0xFFFFFFFF,
                       boot.encode())
    for off, length, sd in table:
        head += struct.pack(FILE_FMT, off, length, sd.encode())
    head += bytes(MAXFILE - len(table)) * struct.calcsize(FILE_FMT)

    assert len(head) == HDR_SIZE, len(head)
    return bytes(head) + bytes(PAYLOAD_OFF - HDR_SIZE) + bytes(payload)


def unpack(img):
    """Re-read a packed image the way n8push.c does.  Returns (hdr, files)."""
    magic, version, nfiles, poff, pbytes, crc, boot = struct.unpack_from(HDR_FMT, img, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    if version != VERSION:
        raise ValueError(f"version {version}, expected {VERSION}")
    if not 1 <= nfiles <= MAXFILE:
        raise ValueError(f"nfiles {nfiles}")
    if poff < HDR_SIZE or poff + pbytes > len(img):
        raise ValueError(f"payload {poff}+{pbytes} outside a {len(img)} B image")
    if zlib.crc32(img[poff:poff + pbytes]) & 0xFFFFFFFF != crc:
        raise ValueError("payload crc mismatch")

    base = struct.calcsize(HDR_FMT)
    files = []
    for i in range(nfiles):
        off, length, path = struct.unpack_from(FILE_FMT, img, base + i * struct.calcsize(FILE_FMT))
        if off + length > pbytes:
            raise ValueError(f"file {i} runs past the payload")
        if b"\0" not in path:
            raise ValueError(f"file {i} path is not terminated")
        files.append((path.split(b"\0")[0].decode(), img[poff + off:poff + off + length]))

    hdr = dict(version=version, nfiles=nfiles, payload_off=poff,
               payload_bytes=pbytes, crc32=crc,
               boot=boot.split(b"\0")[0].decode())
    return hdr, files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pairs", nargs="*", metavar="LOCAL=SD",
                    help="a local file and where it lands on the cart's SD")
    ap.add_argument("--boot", default="", help="menu_install target; empty pushes without booting")
    ap.add_argument("--verify", action="store_true", help="re-read the packed image")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    entries = []
    boot = args.boot

    for pair in args.pairs:
        if "=" not in pair:
            raise SystemExit(f"expected LOCAL=SD, got {pair!r}")
        local, sd = pair.split("=", 1)
        entries.append((local, sd))

    for local, _ in entries:
        if not os.path.isfile(local):
            raise SystemExit(f"no such file: {local}")

    img = pack(entries, boot)
    with open(args.out, "wb") as fh:
        fh.write(img)

    print(f"{args.out}: {len(img)} B, {len(entries)} file(s)")
    for local, sd in entries:
        print(f"  {os.path.basename(local):<28} -> {sd}  ({os.path.getsize(local)} B)")
    print(f"  boot: {boot or 'none'}")

    if args.verify:
        hdr, files = unpack(img)
        for (local, sd), (got_sd, blob) in zip(entries, files):
            with open(local, "rb") as fh:
                assert blob == fh.read(), local
            assert got_sd == sd, (got_sd, sd)
        assert hdr["boot"] == boot
        print(f"  verify: {len(files)} file(s) byte-exact, crc {hdr['crc32']:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
