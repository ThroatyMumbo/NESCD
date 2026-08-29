#!/usr/bin/env python3
"""Check a mastered CDTRACK image the way `src/track.c` reads it.

mktrack's own --verify checks the blocks it just built.  This re-reads the
packed image cold, using the same field offsets and the same fold arithmetic
the firmware uses, so a struct-layout or offset mismatch between the two ends
shows up here rather than on the bench.

    test_track.py TRACK.rom [--audio SOURCE]

The loop checks are the ones that matter.  "block ls_blk on lap 2 decodes
identically to block ls_blk cold" is what makes the seam sample-exact; get it
wrong and lap 1 sounds perfect while every later one drifts.
"""
import argparse
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audiofmt import (AFMT_ADPCM4, AFMT_PCM16, AFMT_STR,      # noqa: E402
                      AUDIO_CH, AUDIO_RATE, AUDIO_SPR, TRK_HDR_FMT,
                      TRK_HDR_LEN, audio_ok, block_bytes, check_block,
                      decode_block_ref, decode_blocks, loop_block,
                      loop_geometry)

HDR_FMT   = TRK_HDR_FMT
HDR_LEN   = TRK_HDR_LEN
SNR_FLOOR = 12.0


def check_track(img, audio=None):
    """An CDTRACK image, by track.c's own rules: no records, nframes counts
    audio blocks, `flags` bit 0 = loop.  Returns the header as a dict so a
    caller embedding the image can size it."""
    (magic, version, nblocks, nslices, fpf, nrecords, table_off, payload_off,
     payload_bytes, crc, _pal, flags, afmt, arate, ach, aspr, ablk, aoff,
     abytes, acrc, ls, le, lead) = struct.unpack_from(HDR_FMT, img)

    assert magic == b"CDTRACK\0", "not a CDTRACK image"
    assert version == 2 and (nslices, fpf, nrecords) == (0, 0, 0)
    assert nblocks >= 1, "a track holds at least one block"
    assert table_off == HDR_LEN and payload_off == table_off + 4
    assert payload_bytes == 0 and crc == zlib.crc32(b"")
    assert struct.unpack_from("<I", img, table_off) == (0,), "table sentinel"
    assert afmt in (AFMT_PCM16, AFMT_ADPCM4), "a track cannot be silent"
    assert (aspr, arate, ach) == (AUDIO_SPR, AUDIO_RATE, AUDIO_CH)
    assert ablk == block_bytes(afmt, aspr)
    assert abytes == nblocks * ablk and aoff == payload_off
    assert aoff + abytes == len(img), "image has trailing bytes"
    assert zlib.crc32(img[aoff:aoff + abytes]) == acrc, "audio crc mismatch"
    # track.c's loop-range rules, plus: the masterer stores nothing past it.
    if le:
        assert flags & 1, "a loop range on a one-shot track"
        assert ls % aspr == 0, "loop_start is not block-aligned"
        assert ls < le <= nblocks * aspr, "loop range outside the array"
        assert (nblocks - 1) * aspr < le, "blocks past loop_end are never played"
        assert lead < aspr, "lead-in of a block or more"
    else:
        assert ls == 0 and lead == 0, "loop words without a range"
    print(f"  header ok: track, {nblocks} blocks "
          f"({nblocks * aspr / AUDIO_RATE:.1f} s), "
          f"{'loop' if flags & 1 else 'once'}"
          + (f" [{ls / AUDIO_RATE:.3f}, {le / AUDIO_RATE:.3f}) s after a "
             f"{lead}-sample lead-in" if le else ""))
    check_audio(img, afmt, nblocks, aoff, ablk, aspr, audio, (ls, le), lead)
    return dict(nblocks=nblocks, afmt=afmt, aspr=aspr, ablk=ablk, aoff=aoff,
                abytes=abytes, loop=bool(flags & 1), loop_start=ls,
                loop_end=le, lead_in=lead)


def check_audio(img, afmt, nframes, aoff, ablk, aspr, audio=None,
                loop=(0, 0), lead=0):
    """The stored blocks, walked by the firmware's own fold (bgmloop.h; a
    track with no range is that fold's 0/0 case)."""
    geo = loop_geometry(loop[0], loop[1], aspr, nframes)

    def blk(seq):
        i = loop_block(geo, seq)
        return img[aoff + i * ablk:aoff + (i + 1) * ablk]

    for seq in range(nframes):
        check_block(afmt, blk(seq), aspr)

    got = decode_blocks(afmt, img[aoff:aoff + nframes * ablk], nframes, aspr)
    # The vectorised decoder against the plain-Python reference, on a sample:
    # the tests must not lean entirely on the path they are checking.
    for i in (0, nframes // 2, nframes - 1):
        assert np.array_equal(got[i], decode_block_ref(afmt, blk(i), aspr)), \
            f"block {i}: standalone reference decode differs"

    # The loop seam. The first block of lap 2 must decode identically to the
    # same block played cold, or every lap drifts from the first.
    ls_blk, le_blk = geo[0], geo[1]
    assert np.array_equal(decode_block_ref(afmt, blk(le_blk + 1), aspr),
                          got[ls_blk]), \
        f"loop seam: block {ls_blk} on lap 2 differs from lap 1"

    line = (f"  audio ok: {AFMT_STR[afmt]}, {nframes} blocks of {ablk} B, "
            f"{aspr} samples each, self-contained, seam exact")
    if audio:
        from audio_source import open_audio
        with open_audio(audio, spr=aspr) as a:
            want = np.concatenate([np.zeros(lead, np.int16)]
                                  + [a.read() for _ in range(nframes)])
        want = want[:nframes * aspr]
        floor = float("inf") if afmt == AFMT_PCM16 else SNR_FLOOR
        ok, s, rms, err = audio_ok(want, got.reshape(-1), floor)
        assert ok, (f"audio SNR {s:.1f} dB, error {err:.2f} LSB rms "
                    f"against a source at {rms:.1f} rms")
        line += f", SNR {s:.1f} dB" if s != float("inf") else ", byte-exact"
    print(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--audio", help="source to compare the decoded blocks against")
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        img = fh.read()
    print(f"{args.rom}: {len(img)} bytes")
    check_track(img, args.audio)
    print("  all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
