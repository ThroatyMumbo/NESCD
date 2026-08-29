#!/usr/bin/env python3
"""Master an audio file into a CDTRACK image: background music the PGA2350
plays during gameplay.

    0    96                header (TRK_HDR_FMT, magic "CDTRACK\\0")
    96   4                 one-entry offset table: the empty payload's sentinel
    100  nframes * block   one self-contained audio block per slot

`nframes` counts audio blocks, `nrecords` is 0, and the `flags` word carries
bit 0 = loop.  The player free-runs at the mastered 44100 Hz, so these blocks
play at pitch.

A loop range (--loop-start/--loop-end, or a tracks.csv row) lands in the
header's loop_start/loop_end words, in samples.  The player folds its block
cursor onto [loop_start, loop_end) (`src/bgmloop.h`) and only ever enters a
block at its start, so the source is led in with up to a block of silence to
put loop_start on a block boundary; lead_in records how much.  Nothing past
loop_end is stored.
"""
import argparse
import csv
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_source import open_audio                            # noqa: E402
from audiofmt import (AFMT_NAMES, AFMT_PCM16, AFMT_STR,        # noqa: E402
                      AUDIO_CH, AUDIO_RATE, AUDIO_SPR, TRK_HDR_FMT,
                      TRK_HDR_LEN, audio_ok, block_bytes, decode_blocks,
                      encode_blocks, loop_fade, loop_fade_range,
                      loop_geometry, loop_unroll)

HDR_FMT, HDR_LEN = TRK_HDR_FMT, TRK_HDR_LEN

MAGIC     = b"CDTRACK\0"
VERSION   = 2
F_LOOP    = 1
SNR_FLOOR = 12.0


def load_samples(media, spr, gain_db, blocks_cap):
    """Up to blocks_cap blocks of the source, the tail zero-padded to a whole
    block; also how many real samples the pipe held."""
    src = open_audio(media, spr=spr, gain_db=gain_db, required=True)
    print(f"  {src.desc}")
    blocks = []
    while blocks_cap is None or len(blocks) < blocks_cap:
        before = src.pad_blocks
        b = src.read()
        if src.pad_blocks > before:
            if src.nsamples % spr:
                blocks.append(b)         # the padded real tail
            break
        blocks.append(b)
    src.close()
    if not blocks:
        raise SystemExit(f"{media}: no audio samples")
    return np.concatenate(blocks), src.nsamples


def csv_loop(path, media):
    """(loop_start, loop_end) seconds from the row named after the source's
    file stem: `name,loop_start,loop_end`."""
    stem = os.path.splitext(os.path.basename(media))[0]
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            if row["name"] == stem:
                return float(row["loop_start"]), float(row["loop_end"])
    raise SystemExit(f"{path}: no row named {stem}")


def pack(afmt, spr, nblocks, blob, flags, loop_start, loop_end, lead_in):
    table_off = HDR_LEN
    payload_off = table_off + 4                  # one sentinel entry, 4-aligned
    head = struct.pack(HDR_FMT, MAGIC, VERSION, nblocks, 0, 0, 0, table_off,
                       payload_off, 0, zlib.crc32(b""), 0, flags,
                       afmt, AUDIO_RATE, AUDIO_CH, spr,
                       block_bytes(afmt, spr), payload_off, len(blob),
                       zlib.crc32(blob), loop_start, loop_end, lead_in)
    return head + struct.pack("<I", 0) + blob


def main():
    ap = argparse.ArgumentParser(
        description="Master an audio file into a flashable CDTRACK image")
    ap.add_argument("audio", help="any source ffmpeg can decode")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--audio-format", choices=("pcm16", "adpcm"),
                    default="adpcm")
    ap.add_argument("--audio-gain", type=float,
                    help="dB applied after the resample")
    ap.add_argument("--seconds", type=float,
                    help="cap the track length (a minute of adpcm is 1.3 MiB)")
    ap.add_argument("--once", action="store_true",
                    help="play through once instead of looping")
    ap.add_argument("--loop-start", type=float, metavar="SEC",
                    help="where the loop restarts (the song starts at 0)")
    ap.add_argument("--loop-end", type=float, metavar="SEC",
                    help="where it wraps; nothing past it is stored")
    ap.add_argument("--loop-csv", metavar="FILE",
                    help="both points from the row named after the source: "
                         "name,loop_start,loop_end")
    ap.add_argument("--loop-fade", type=float, default=0.0,
                    help="ms of equal-power crossfade across the loop seam")
    ap.add_argument("--verify", action="store_true",
                    help="decode the packed blocks back and check the SNR")
    args = ap.parse_args()

    afmt = AFMT_NAMES[args.audio_format]
    spr = AUDIO_SPR
    fade = int(args.loop_fade * AUDIO_RATE / 1000)

    ls_sec, le_sec = args.loop_start, args.loop_end
    if args.loop_csv:
        ls_sec, le_sec = csv_loop(args.loop_csv, args.audio)
    ranged = le_sec is not None
    if ranged:
        if args.once or args.seconds is not None:
            raise SystemExit("a loop range excludes --once and --seconds")
        ls = int(round((ls_sec or 0.0) * AUDIO_RATE))
        le = int(round(le_sec * AUDIO_RATE))
        if not 0 <= ls < le:
            raise SystemExit(f"loop [{ls_sec}, {le_sec}) s is empty")
        lead = (-ls) % spr                  # lands loop_start on a block
        ls, le = ls + lead, le + lead
        nblocks = -(-le // spr)
        need = le + (fade if ls < fade else 0)   # a head fade reads past le
        samples, nreal = load_samples(args.audio, spr, args.audio_gain,
                                      -(-(need - lead) // spr) + 1)
        if lead + nreal < le:
            raise SystemExit(f"{args.audio}: ends at {nreal / AUDIO_RATE:.3f} s, "
                             f"before loop_end {le_sec} s")
        samples = np.concatenate((np.zeros(lead, np.int16), samples))
        if fade:
            samples = loop_fade_range(samples, ls, le, fade)
        samples = np.concatenate((samples, np.zeros(nblocks * spr, np.int16)))
        samples = samples[:nblocks * spr]
    else:
        if args.loop_start is not None:
            raise SystemExit("--loop-start needs --loop-end")
        ls = le = lead = 0
        cap = (None if args.seconds is None
               else max(1, round(args.seconds * AUDIO_RATE / spr)))
        samples, _ = load_samples(args.audio, spr, args.audio_gain, cap)
        if not args.once and fade:
            samples = loop_fade(samples, fade)
        nblocks = len(samples) // spr

    blob = encode_blocks(afmt, samples, spr)
    blk = block_bytes(afmt, spr)
    flags = 0 if args.once else F_LOOP
    print(f"  track: {AFMT_STR[afmt]} {AUDIO_RATE} mono, {nblocks} blocks of "
          f"{blk} B ({nblocks * spr / AUDIO_RATE:.1f} s), "
          f"{'loop' if flags & F_LOOP else 'once'}")
    if ranged:
        ls_blk, le_blk, le_off, lap = loop_geometry(ls, le, spr, nblocks)
        print(f"  loop: [{ls / AUDIO_RATE:.3f}, {le / AUDIO_RATE:.3f}) s after a "
              f"{lead}-sample lead-in = samples [{ls}, {le}), blocks "
              f"{ls_blk}..{le_blk} ({lap} a lap, the last {le_off} samples long)")

    if args.verify:
        got = decode_blocks(afmt, blob, nblocks, spr)
        if ranged:
            have = loop_unroll(got, loop_geometry(ls, le, spr, nblocks), spr, 2)
            want = np.concatenate((samples[:le], samples[ls:le], samples[ls:le]))
        else:
            have = got.reshape(-1)
            want = samples[:nblocks * spr]
        assert len(have) == len(want), (len(have), len(want))
        floor = float("inf") if afmt == AFMT_PCM16 else SNR_FLOOR
        ok, snr, rms, err = audio_ok(want, have, floor)
        print("  verify: " + ("byte-exact" if snr == float("inf")
                              else f"SNR {snr:.1f} dB")
              + (", two laps unrolled" if ranged else ""))
        if not ok:
            raise SystemExit(f"  VERIFY FAILED: audio SNR {snr:.1f} dB, error "
                             f"{err:.2f} LSB rms at {rms:.1f} rms")

    img = pack(afmt, spr, nblocks, blob, flags, ls, le, lead)
    with open(args.out, "wb") as f:
        f.write(img)
    print(f"  wrote {args.out}: {len(img)} bytes ({len(img) / 1048576:.2f} MiB)")


if __name__ == "__main__":
    main()
