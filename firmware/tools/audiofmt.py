"""The block audio format a disc carries, and the loop fold that plays it.

`src/audiofmt.h`, `src/ima.h` and `src/bgmloop.h` are the firmware's copies of
these numbers; nothing here may drift from them.

Audio is a stream of self-contained blocks of exactly `AUDIO_SPR` mono samples.
Self-contained means the block header carries decoder state rather than sample
0, so the producer can join the array at any block -- which is what a track
resuming at its saved cursor, or wrapping at its loop point, has to do.

Mono throughout: the DAC's line out is destined for NES expansion port pin 3,
and the firmware duplicates each sample to both DAC channels.
"""
import struct

import numpy as np

import ima_adpcm

AUDIO_RATE = 44100               # a track free-runs at the mastered rate
AUDIO_SPR  = 1470                # samples per block, exactly 44100 / 30
AUDIO_CH   = 1

# The 96-byte CDTRACK header; src/track.h is the firmware's struct.
TRK_HDR_FMT = "<8s22I"
TRK_HDR_LEN = 96

AFMT_NONE   = 0
AFMT_PCM16  = 1                  # mono s16le, 2 * spr bytes
AFMT_ADPCM4 = 2                  # ima adpcm, see ima_adpcm.py

AFMT_NAMES = {"none": AFMT_NONE, "pcm16": AFMT_PCM16, "adpcm": AFMT_ADPCM4}
AFMT_STR   = {v: k for k, v in AFMT_NAMES.items()}


def block_bytes(fmt, spr=AUDIO_SPR):
    if fmt == AFMT_NONE:
        return 0
    if fmt == AFMT_PCM16:
        return 2 * spr
    if fmt == AFMT_ADPCM4:
        return ima_adpcm.block_bytes(spr)
    raise ValueError(f"unknown audio format {fmt}")


def encode_blocks(fmt, samples, spr=AUDIO_SPR):
    """`len(samples) // spr` whole blocks.  A short tail is dropped, which is
    what the callers' floor division does with video frames too."""
    if fmt == AFMT_NONE:
        return b""
    n = len(samples) // spr
    if fmt == AFMT_PCM16:
        return np.asarray(samples[:n * spr], np.int16).tobytes()
    return ima_adpcm.encode(samples, spr)


def decode_blocks(fmt, buf, nblocks, spr=AUDIO_SPR):
    """-> (nblocks, spr) int16."""
    if fmt == AFMT_PCM16:
        return np.frombuffer(buf, "<i2", count=nblocks * spr).reshape(nblocks, spr)
    if fmt == AFMT_ADPCM4:
        return ima_adpcm.decode_blocks(buf, nblocks, spr)
    raise ValueError(f"cannot decode audio format {fmt}")


def decode_block_ref(fmt, block, spr=AUDIO_SPR):
    """One block, via the plain-Python reference path."""
    if fmt == AFMT_PCM16:
        return np.frombuffer(block, "<i2", count=spr)
    return ima_adpcm.decode_block_ref(block, spr)


def check_block(fmt, block, spr=AUDIO_SPR):
    """The structural invariants a mastered block must hold.  Raises."""
    blk = block_bytes(fmt, spr)
    if len(block) < blk:
        raise ValueError(f"block is {len(block)} B, want {blk}")
    if fmt != AFMT_ADPCM4:
        return
    if block[2] > 88:
        raise ValueError(f"step_index {block[2]} > 88")
    if block[3] != 0:
        raise ValueError("reserved header byte is not zero")
    if any(block[ima_adpcm.HDR_LEN + (spr + 1) // 2:blk]):
        raise ValueError("block pad is not zero")


def snr_db(ref, got):
    ref, got = np.asarray(ref, np.float64), np.asarray(got, np.float64)
    noise = ((ref - got) ** 2).mean()
    if noise == 0:
        return float("inf")
    sig = (ref ** 2).mean()
    return float("-inf") if sig == 0 else 10.0 * np.log10(sig / noise)


# Below this the source is silence as far as a 4-bit codec is concerned
# (-54 dBFS), and a ratio of two near-zero numbers says nothing about the
# decoder. 32 LSB of error is -60 dBFS: inaudible, three times the worst real
# content here has produced, and orders of magnitude tighter than anything a
# broken decoder gives.
QUIET_RMS = 64.0
QUIET_ERR = 32.0


def audio_ok(ref, got, floor):
    """(ok, snr_db, rms, err_rms) for one span, judged the right way for its
    level: SNR where there is signal, absolute error where there is not.

    A flat SNR floor fails on digital silence -- real sources have passages at
    RMS 2 -- while a lone absolute bound would let real signal degrade unseen.
    """
    ref = np.asarray(ref, np.float64)
    got = np.asarray(got, np.float64)
    rms = float(np.sqrt((ref ** 2).mean()))
    err = float(np.sqrt(((ref - got) ** 2).mean()))
    s = snr_db(ref, got)
    if floor == float("inf"):
        ok = err == 0.0                  # pcm16 is exact at every level
    else:
        ok = err <= QUIET_ERR if rms < QUIET_RMS else s >= floor
    return ok, s, rms, err


def loop_fade(audio, n):
    """Equal-power crossfade of the last n samples into the first, so the loop
    seam does not click.  Sample-exactness makes the seam land in the right
    place; it does not stop the waveform jumping across it."""
    n = min(n, len(audio) // 4)
    if n <= 0:
        return audio
    out = np.array(audio, np.float64)
    t = (np.arange(n) + 0.5) / n
    a, b = np.cos(t * np.pi / 2), np.sin(t * np.pi / 2)
    out[:n] = out[:n] * b + out[-n:] * a
    return np.clip(np.round(out), -32768, 32767).astype(np.int16)


def loop_fade_range(audio, ls, le, n):
    """The same across a [ls, le) loop, leaving lap 1 intact: the pre-loop
    material blends into the tail, or with less than n samples before ls the
    post-loop material blends into the head instead."""
    n = min(n, (le - ls) // 4)
    if n <= 0:
        return audio
    out = np.array(audio, np.float64)
    t = (np.arange(n) + 0.5) / n
    a, b = np.cos(t * np.pi / 2), np.sin(t * np.pi / 2)
    if ls >= n:
        out[le - n:le] = out[le - n:le] * a + out[ls - n:ls] * b
    else:
        m = min(n, len(out) - le)
        if m <= 0:
            return audio
        out[ls:ls + m] = out[ls:ls + m] * b[:m] + out[le:le + m] * a[:m]
    return np.clip(np.round(out), -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------------------
# The player's loop fold, mirrored from src/bgmloop.h
# ---------------------------------------------------------------------------

def loop_geometry(ls, le, spr, nblocks):
    """(ls_blk, le_blk, le_off, lap) for a [ls, le) range in samples; 0/0 is
    the whole array."""
    if not le:
        ls, le = 0, nblocks * spr
    ls_blk, le_blk = ls // spr, (le - 1) // spr
    return ls_blk, le_blk, le - le_blk * spr, le_blk - ls_blk + 1


def loop_block(geo, seq):
    ls_blk, le_blk, _, lap = geo
    return seq if seq <= le_blk else ls_blk + (seq - le_blk - 1) % lap


def loop_end_off(geo, seq, spr):
    return geo[2] if loop_block(geo, seq) == geo[1] else spr


def loop_unroll(decoded, geo, spr, laps):
    """The samples the player emits from a decoded (nblocks, spr) array over
    lap 0 plus `laps` more, visit by visit."""
    _ls_blk, le_blk, _, lap = geo
    return np.concatenate([decoded[loop_block(geo, seq)][:loop_end_off(geo, seq, spr)]
                           for seq in range(le_blk + 1 + laps * lap)])


def wav_bytes(samples, rate=AUDIO_RATE):
    """A 44-byte RIFF/WAVE PCM16 mono header plus the samples.

    Deliberately plain PCM, not IMA WAV: ffmpeg's `adpcm_ima_wav` derives its
    sample count from the block size as `1 + (block_align - 4) / 4 * 8`, so it
    can never agree with a 1470-sample block.
    """
    pcm = np.asarray(samples, np.int16).tobytes()
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
            + b"data" + struct.pack("<I", len(pcm)) + pcm)


# ---------------------------------------------------------------------------
# Extraction: pull the audio back out of a mastered image as a .wav
# ---------------------------------------------------------------------------

def _track_blocks(img):
    """(fmt, spr, block_bytes, [block...]) from a CDTRACK image, walked by
    exactly the indexing the firmware uses."""
    f = struct.unpack_from(TRK_HDR_FMT, img)
    (_, ver, nframes, _nsl, _fpf, _nrec, _toff, _poff, _pb, _crc, _pal, _rsv,
     afmt, _arate, _ach, aspr, ablk, aoff, abytes, _acrc) = f[:20]
    if ver != 2:
        raise SystemExit(f"rom version {ver}, want 2")
    if afmt == AFMT_NONE:
        raise SystemExit("image carries no audio")
    if abytes != nframes * ablk:
        raise SystemExit(f"audio_bytes {abytes} != nframes {nframes} * {ablk}")
    return afmt, aspr, ablk, [img[aoff + i * ablk:aoff + (i + 1) * ablk]
                              for i in range(nframes)]


def _game_blocks(img, track_id):
    """One track item of an NESCDISC disc, by id; None takes the first."""
    from mkgamedisc import ITEM_TRACK, SECTOR, read_catalog
    _hdr, items = read_catalog(img)
    tracks = [i for i in items if i["type"] == ITEM_TRACK]
    if not tracks:
        raise SystemExit("the disc holds no track")
    if track_id is None:
        it = tracks[0]
    else:
        hit = [i for i in tracks if i["id"] == track_id]
        if not hit:
            raise SystemExit(f"no track with id {track_id}")
        it = hit[0]
    at = it["lba"] * SECTOR
    return _track_blocks(bytes(img[at:at + it["sectors"] * SECTOR]))


def image_blocks(img, track_id=None):
    if img[:8] == b"CDTRACK\0":
        return _track_blocks(img)
    if img[:8] == b"NESCDISC":
        return _game_blocks(img, track_id)
    raise SystemExit("not a CDTRACK or NESCDISC image")


if __name__ == "__main__":
    import argparse
    import mmap
    ap = argparse.ArgumentParser(
        description="Extract a mastered track's audio back out as a .wav, "
                    "walked by the same block indexing the firmware uses")
    ap.add_argument("image")
    ap.add_argument("-o", "--out", help="write a .wav (omit for a report)")
    ap.add_argument("--from", dest="first", type=int, default=0,
                    help="first block")
    ap.add_argument("--count", type=int, help="blocks (default: to the end)")
    ap.add_argument("--track", type=int,
                    help="NESCDISC only: which track id (default: the first)")
    a = ap.parse_args()

    with open(a.image, "rb") as fh:
        img = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        fmt, spr, blk, blocks = image_blocks(img, a.track)
        sel = blocks[a.first:None if a.count is None else a.first + a.count]
        for b in sel:
            check_block(fmt, b, spr)
        pcm = decode_blocks(fmt, b"".join(sel), len(sel), spr).reshape(-1)

        secs = len(sel) * spr / AUDIO_RATE
        peak = int(np.abs(pcm.astype(np.int32)).max()) if len(pcm) else 0
        rms = float(np.sqrt((pcm.astype(np.float64) ** 2).mean())) if len(pcm) else 0.0
        print(f"  {AFMT_STR[fmt]}, {spr} spr, {blk} B/block, "
              f"{len(sel)} of {len(blocks)} blocks from {a.first}")
        print(f"  {len(pcm)} samples, {secs:.2f} s at {AUDIO_RATE} Hz")
        print(f"  peak {peak} ({20 * np.log10(max(peak, 1) / 32768):.1f} dBFS), "
              f"rms {rms:.0f} ({20 * np.log10(max(rms, 1) / 32768):.1f} dBFS)")
        if a.out:
            with open(a.out, "wb") as w:
                w.write(wav_bytes(pcm))
            print(f"  wrote {a.out}")
        img.close()
