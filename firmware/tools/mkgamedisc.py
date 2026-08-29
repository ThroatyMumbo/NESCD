#!/usr/bin/env python3
"""Assemble a game disc: the ROM and its music on one CD image the PGA2350
boots and serves by LBA.

    mkgamedisc.py -o game.img --title "MY GAME" --nes game.nes \\
                  --track 1=song.rom --track 2=boss.rom

Sector 0 is the catalog; everything else is an existing container placed
whole at a sector boundary, so every masterer and every test tool is reused:

    LBA 0        NESCDISC header (HDR_FMT) + item table (ITEM_FMT each)
    LBA 1..15    reserved (the ISO9660 system area, kept free for a hybrid)
    LBA 16..     items: the ROM (N8PUSH1), then tracks (CDTRACK), each
                 zero-padded to a sector, crc over the pad

The ROM comes first because it is read once at insert.  A track's id is the
value the game writes to its music mailbox at $1FF9.

Item type 2 is reserved for items this firmware does not serve: a disc
carrying one still opens, and the item is listed and refused.
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkn8push                                              # noqa: E402
import test_track                                            # noqa: E402

MAGIC     = b"NESCDISC"
VERSION   = 2
SECTOR    = 2048
HDR_FMT   = "<8sI32sI4I"          # magic, version, title, nitems, reserved[4]
ITEM_FMT  = "<8I"                 # type, id, lba, sectors, crc32, reserved[3]
HDR_LEN   = struct.calcsize(HDR_FMT)
ITEM_LEN  = struct.calcsize(ITEM_FMT)
MAXITEMS  = 32
TITLE_LEN = 32
FIRST_LBA = 16
CDR_SECTORS = 359846              # an 80-minute CD-R

ITEM_ROM, ITEM_RESERVED, ITEM_TRACK = 1, 2, 3
ITEM_NAME = {ITEM_ROM: "rom", ITEM_RESERVED: "reserved", ITEM_TRACK: "track"}
TRACK_MAX = 32                    # BGM_TRACK_MAX in main.c

assert HDR_LEN == 64 and ITEM_LEN == 32
assert HDR_LEN + MAXITEMS * ITEM_LEN <= SECTOR


def sectors_of(nbytes):
    return -(-nbytes // SECTOR)


def nes_mapper(path):
    with open(path, "rb") as fh:
        hdr = fh.read(16)
    if hdr[:4] != b"NES\x1a":
        raise SystemExit(f"{path}: not an iNES/NES 2.0 file")
    return (hdr[7] & 0xF0) | (hdr[6] >> 4)


def rom_item(args):
    """The N8PUSH1 image the cart is loaded from: just the ROM.

    The cart's menu resolves the mapper from the iNES header and loads a core
    itself, so a stock-mapper ROM is the whole item.
    """
    if args.n8p:
        with open(args.n8p, "rb") as fh:
            img = fh.read()
        hdr, files = mkn8push.unpack(img)
        if not hdr["boot"]:
            raise SystemExit(f"{args.n8p}: names no boot target")
        if hdr["boot"] not in [n for n, _ in files]:
            raise SystemExit(f"{args.n8p}: boot {hdr['boot']!r} is not one of "
                             "the files inside")
        return img, args.n8p
    if not args.nes:
        raise SystemExit("--nes is required (or --n8p)")
    print(f"  rom mapper {nes_mapper(args.nes)}")
    entries = [(args.nes, args.sd_nes)]
    return mkn8push.pack(entries, boot=args.sd_nes), args.nes


def track_item(path):
    with open(path, "rb") as fh:
        img = fh.read()
    if img[:8] != b"CDTRACK\0":
        raise SystemExit(f"{path}: not a CDTRACK image")
    t = test_track.check_track(img)
    return img, t


def parse_items(pairs, kind, id_max):
    out = []
    for p in pairs or ():
        if "=" not in p:
            raise SystemExit(f"expected ID=FILE, got {p!r}")
        sid, path = p.split("=", 1)
        try:
            iid = int(sid)
        except ValueError:
            raise SystemExit(f"{kind} id must be a number: {p!r}")
        if not 1 <= iid <= id_max:
            raise SystemExit(f"{kind} id {iid} outside 1..{id_max}")
        if not os.path.isfile(path):
            raise SystemExit(f"no such file: {path}")
        out.append((iid, path))
    ids = [i for i, _ in out]
    if len(ids) != len(set(ids)):
        raise SystemExit(f"duplicate {kind} id")
    return sorted(out, key=lambda t: t[0])


def read_catalog(img):
    """(hdr, [item dict...]) from the first sector; the cold-read twin of
    catalog_check() in the firmware.  Raises on anything it would refuse."""
    magic, ver, title, nitems, *rsv = struct.unpack_from(HDR_FMT, img, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    if ver != VERSION:
        raise ValueError(f"version {ver}, want {VERSION}")
    if not 1 <= nitems <= MAXITEMS:
        raise ValueError(f"nitems {nitems}")
    if b"\0" not in title:
        raise ValueError("title is not terminated")
    items = []
    for i in range(nitems):
        typ, iid, lba, sectors, crc, *r = struct.unpack_from(
            ITEM_FMT, img, HDR_LEN + i * ITEM_LEN)
        items.append(dict(type=typ, id=iid, lba=lba, sectors=sectors, crc32=crc,
                          reserved=tuple(r)))
    hdr = dict(version=ver, title=title.split(b"\0")[0].decode(),
               nitems=nitems, reserved=tuple(rsv))
    return hdr, items


def pack_catalog(title, items):
    """items: [(type, id, lba, sectors, crc32)] -> one sector."""
    t = title.encode()
    if len(t) >= TITLE_LEN:
        raise SystemExit(f"title is over {TITLE_LEN - 1} bytes")
    head = struct.pack(HDR_FMT, MAGIC, VERSION, t, len(items), 0, 0, 0, 0)
    for typ, iid, lba, sectors, crc in items:
        head += struct.pack(ITEM_FMT, typ, iid, lba, sectors, crc, 0, 0, 0)
    return head + b"\0" * (SECTOR - len(head))


def write_items(out, blobs):
    """Stream each (type, id, bytes-or-path, nbytes) to `out` from LBA
    FIRST_LBA, padded to sectors.  Returns the catalog entries."""
    out.seek(FIRST_LBA * SECTOR)
    lba = FIRST_LBA
    cat = []
    for typ, iid, src, nbytes in blobs:
        crc, done = 0, 0
        pad = sectors_of(nbytes) * SECTOR - nbytes
        if isinstance(src, (bytes, bytearray)):
            chunks = [src]
        else:
            chunks = iter(lambda: src.read(1 << 22), b"")
        for chunk in chunks:
            out.write(chunk)
            crc = zlib.crc32(chunk, crc)
            done += len(chunk)
        assert done == nbytes, (done, nbytes)
        if pad:
            out.write(b"\0" * pad)
            crc = zlib.crc32(b"\0" * pad, crc)
        cat.append((typ, iid, lba, sectors_of(nbytes), crc & 0xFFFFFFFF))
        lba += sectors_of(nbytes)
    return cat


def main():
    ap = argparse.ArgumentParser(
        description="assemble a game disc image for the PGA2350",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--title", default="NESCD GAME")
    ap.add_argument("--nes", help="the game ROM")
    ap.add_argument("--sd-nes", default="nescd/game.nes",
                    help="where the ROM lands, and what the menu installs")
    ap.add_argument("--n8p", help="a prebuilt N8PUSH1 image instead of --nes")
    ap.add_argument("--track", action="append", metavar="ID=ROM",
                    help=f"a CDTRACK image as track ID (1..{TRACK_MAX})")
    ap.add_argument("--verify", action="store_true",
                    help="re-read the written image cold")
    args = ap.parse_args()

    tracks = parse_items(args.track, "track", TRACK_MAX)
    if 1 + len(tracks) > MAXITEMS:
        raise SystemExit(f"over {MAXITEMS} items")

    rom, rom_src = rom_item(args)
    print(f"  rom: {rom_src}")
    blobs = [(ITEM_ROM, 0, rom, len(rom))]
    srcs = [rom_src]
    for iid, path in tracks:
        img, t = track_item(path)
        rng = (f", loop [{t['loop_start']}, {t['loop_end']}) samples"
               if t["loop_end"] else "")
        print(f"  track {iid}: {path}, {t['nblocks']} blocks, "
              f"{'loop' if t['loop'] else 'once'}{rng}")
        blobs.append((ITEM_TRACK, iid, img, len(img)))
        srcs.append(path)

    total = FIRST_LBA + sum(sectors_of(n) for _, _, _, n in blobs)
    if total > CDR_SECTORS:
        raise SystemExit(f"{total} sectors will not fit an 80-min CD-R "
                         f"({CDR_SECTORS})")

    with open(args.out, "wb") as out:
        out.write(b"\0" * (FIRST_LBA * SECTOR))
        cat = write_items(out, blobs)
        out.seek(0)
        out.write(pack_catalog(args.title, cat))

    print(f"  {args.out}: {args.title!r}, {len(cat)} items, {total} sectors "
          f"({total * SECTOR / 2**20:.1f} MiB, "
          f"{100 * total / CDR_SECTORS:.1f} % of an 80-min CD-R)")
    print(f"  {'type':<6}{'id':>3}{'lba':>8}{'sectors':>9}  {'crc32':<9} source")
    for (typ, iid, lba, sectors, crc), src in zip(cat, srcs):
        print(f"  {ITEM_NAME[typ]:<6}{iid:>3}{lba:>8}{sectors:>9}  {crc:08X}  "
              f"{src}")
    print(f"  burn:   firmware/burn.sh {args.out}")

    if args.verify:
        import test_gamedisc
        with open(args.out, "rb") as fh:
            import mmap
            img = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
            n = test_gamedisc.check_image(img)
            img.close()
        if n:
            raise SystemExit(f"  VERIFY FAILED ({n})")
        print("  verify: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
