"""IMA ADPCM, 4-bit mono: the codec a disc's audio blocks are stored in.

Pinned to the byte, because `src/ima.h` decodes what this writes and the two
have no way to negotiate:

    off  size  field
      0     2  int16le  predictor    decoder state BEFORE sample 0
      2     1  uint8    step_index   0..88
      3     1  uint8    0            reserved
      4     n  nibbles  samples 0..spr-1, LOW nibble first
    4+n     .  uint8    0            pad to a 4-byte block

The header carries decoder *state*, not sample 0, so `spr` samples cost exactly
`spr/2` nibble bytes with no dead nibble and no special case for the first
sample.  That differs from `adpcm_ima_wav`, which cannot decode these blocks
anyway: it derives the sample count from the block size as
`1 + (block_align - 4) / 4 * 8`.

Because every block's header restores the exact state its encoder held, decoding
a block standalone is bit-identical to decoding the whole stream from block 0.
That is what makes a disc group resync and a ROM loop seam exact rather than
approximate.

Digital silence encodes bit-exactly: at index 0 the step is 7, and nibble 0
gives `diff = (1 * 7) >> 3 == 0`.  Silent passages carry no idle noise.
"""
import numpy as np

# libavcodec/adpcm_data.c, verbatim.
INDEX_TABLE = np.array([-1, -1, -1, -1, 2, 4, 6, 8,
                        -1, -1, -1, -1, 2, 4, 6, 8], np.int32)

STEP_TABLE = np.array([
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767], np.int32)

_STEP = STEP_TABLE.tolist()
_IDX  = INDEX_TABLE.tolist()

HDR_LEN = 4


def block_bytes(spr):
    """Bytes one block occupies, padded to a 4-byte boundary so the int16
    predictor read is aligned on the RP2350."""
    return (HDR_LEN + (spr + 1) // 2 + 3) & ~3


# ---------------------------------------------------------------------------
# The decode step.  Every other function in this file is built on it.
# ---------------------------------------------------------------------------

def step_scalar(pred, index, nib):
    """One nibble -> (sample, pred, index).

    The multiply form, not the reference implementation's bit-test
    accumulation.  The two differ by up to 1 LSB per sample, and since the
    predictor is fed back that error diverges without bound; this form is what
    ffmpeg's `adpcm_ima_expand_nibble` computes, so it can be cross-checked.
    `>> 3`, never `/ 8`.
    """
    step = _STEP[index]                      # read BEFORE index is updated
    index += _IDX[nib]
    index = 0 if index < 0 else (88 if index > 88 else index)

    diff = ((2 * (nib & 7) + 1) * step) >> 3
    pred = pred - diff if (nib & 8) else pred + diff
    pred = -32768 if pred < -32768 else (32767 if pred > 32767 else pred)
    return pred, pred, index


def decode_block_ref(block, spr):
    """One block -> `spr` samples, in plain Python.  The reference the tests
    check the vectorised path against."""
    pred = int(np.frombuffer(block[0:2], "<i2")[0])
    index = block[2]
    if not 0 <= index <= 88:
        raise ValueError(f"step_index {index} out of range")
    out = np.empty(spr, np.int16)
    for i in range(spr):
        b = block[HDR_LEN + (i >> 1)]
        nib = (b & 0x0F) if (i & 1) == 0 else (b >> 4)
        s, pred, index = step_scalar(pred, index, nib)
        out[i] = s
    return out


def decode_blocks(buf, nblocks, spr, blk=None):
    """`nblocks` blocks -> (nblocks, spr) int16.

    The recursion is sequential in sample index, so the only parallelism is
    across blocks: every block is stepped in lockstep over t.  Blocks are
    independent precisely because each header restores its own state.
    """
    blk = blk or block_bytes(spr)
    raw = np.frombuffer(buf, np.uint8, count=nblocks * blk).reshape(nblocks, blk)

    pred = raw[:, 0:2].copy().view("<i2").reshape(nblocks).astype(np.int32)
    index = raw[:, 2].astype(np.int32)
    if np.any(index > 88):
        raise ValueError("step_index out of range in a block header")

    nibs = raw[:, HDR_LEN:HDR_LEN + (spr + 1) // 2]
    lo, hi = nibs & 0x0F, nibs >> 4
    # Interleave low/high back into sample order without a Python loop.
    n = np.empty((nblocks, lo.shape[1] * 2), np.int32)
    n[:, 0::2], n[:, 1::2] = lo, hi

    out = np.empty((nblocks, spr), np.int16)
    for t in range(spr):
        nib = n[:, t]
        step = STEP_TABLE[index]
        index = np.clip(index + INDEX_TABLE[nib], 0, 88)
        diff = ((2 * (nib & 7) + 1) * step) >> 3
        pred = np.clip(np.where(nib & 8, pred - diff, pred + diff), -32768, 32767)
        out[:, t] = pred
    return out


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------

def _pick_scalar(pred, index, target):
    """The nibble whose DECODED output is nearest `target`.

    Defined in terms of the decoder, over all 8 magnitudes, so encoder and
    decoder cannot disagree and the choice is provably optimal per sample.
    ffmpeg's own encoder truncates `abs(delta) * 4 / step` and so biases the
    magnitude systematically low.
    """
    step = _STEP[index]
    best, bestd = 0, None
    # All 8 positive magnitudes, then all 8 negative -- the same order the
    # vectorised path concatenates them in, so ties break identically.
    for sign in (0, 8):
        for m in range(8):
            diff = ((2 * m + 1) * step) >> 3
            p = pred - diff if sign else pred + diff
            p = -32768 if p < -32768 else (32767 if p > 32767 else p)
            d = abs(p - target)
            if bestd is None or d < bestd:
                best, bestd = m | sign, d
    return best


def encode_ref(samples, spr):
    """Sequential encoder with a true carry across blocks.  Correct and slow;
    the tests diff the vectorised encoder against it."""
    samples = np.asarray(samples, np.int16)
    nblocks = len(samples) // spr
    blk = block_bytes(spr)
    out = bytearray(nblocks * blk)

    pred, index = 0, 0
    for b in range(nblocks):
        base = b * blk
        out[base:base + 2] = int(pred).to_bytes(2, "little", signed=True)
        out[base + 2] = index
        for i in range(spr):
            nib = _pick_scalar(pred, index, int(samples[b * spr + i]))
            _, pred, index = step_scalar(pred, index, nib)
            j = base + HDR_LEN + (i >> 1)
            out[j] |= nib if (i & 1) == 0 else (nib << 4)
    return bytes(out)


def _encode_pass(src, pred0, index0, spr):
    """All blocks in lockstep over t, given per-block start state.

    Returns (nibbles, pred_end, index_end).  Vectorising means each block must
    start from a state of its own; `encode()` gets that state right by running
    this twice.
    """
    nblocks = src.shape[0]
    pred = pred0.astype(np.int32).copy()
    index = index0.astype(np.int32).copy()
    nibs = np.empty((nblocks, spr), np.uint8)

    m = np.arange(8, dtype=np.int32)
    for t in range(spr):
        step = STEP_TABLE[index]
        diff = ((2 * m[None, :] + 1) * step[:, None]) >> 3      # (B, 8)
        cand = np.concatenate((pred[:, None] + diff, pred[:, None] - diff), 1)
        np.clip(cand, -32768, 32767, out=cand)
        nib = np.abs(cand - src[:, t][:, None]).argmin(1).astype(np.int32)
        nibs[:, t] = nib

        step = STEP_TABLE[index]
        index = np.clip(index + INDEX_TABLE[nib], 0, 88)
        d = ((2 * (nib & 7) + 1) * step) >> 3
        pred = np.clip(np.where(nib & 8, pred - d, pred + d), -32768, 32767)
    return nibs, pred, index


def encode(samples, spr, passes=2):
    """`len(samples) // spr` blocks of 4-bit ADPCM.

    Two passes.  The first seeds each block from the previous block's last
    *source* sample with a locally estimated step index; the second re-seeds
    each block with the state the first pass actually ended the previous block
    in, which is the carry a sequential encoder would have had.  Seeding rather
    than guessing matters: a start index a few steps off slews the first handful
    of samples of every block, and at 30 blocks/s that is an audible 30 Hz buzz.
    """
    samples = np.asarray(samples, np.int16)
    nblocks = len(samples) // spr
    blk = block_bytes(spr)
    if nblocks == 0:
        return b""
    src = samples[:nblocks * spr].reshape(nblocks, spr).astype(np.int32)

    # Seed: exact predictor (the previous block's last source sample), and a
    # step whose quarter covers the block's opening mean absolute delta.
    pred0 = np.empty(nblocks, np.int32)
    pred0[0] = 0
    pred0[1:] = src[:-1, -1]
    head = np.abs(np.diff(src[:, :min(33, spr)], axis=1)).mean(1)
    index0 = np.clip(np.searchsorted(STEP_TABLE, np.maximum(head * 4, 7)), 0, 88)

    for p in range(passes):
        nibs, pred_end, index_end = _encode_pass(src, pred0, index0, spr)
        if p + 1 == passes:
            break
        pred0[1:], index0[1:] = pred_end[:-1], index_end[:-1]
        pred0[0], index0[0] = 0, 0

    out = np.zeros((nblocks, blk), np.uint8)
    out[:, 0:2] = pred0.astype("<i2").view(np.uint8).reshape(nblocks, 2)
    out[:, 2] = index0.astype(np.uint8)
    lo, hi = nibs[:, 0::2], nibs[:, 1::2]
    if hi.shape[1] < lo.shape[1]:               # odd spr: a lone low nibble
        hi = np.pad(hi, ((0, 0), (0, 1)))
    packed = lo | (hi << 4)
    out[:, HDR_LEN:HDR_LEN + packed.shape[1]] = packed
    return out.tobytes()
