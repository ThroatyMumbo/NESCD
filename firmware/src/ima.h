// ima.h - the IMA ADPCM decode step, pinned byte-for-byte to
// tools/ima_adpcm.py.
//
// test/host_adpcm.c links this same code and diffs it against the Python
// reference, so the two ends cannot drift.
//
// Two things here are load-bearing and look like style:
//   * the step is read BEFORE step_index is updated;
//   * the difference is the MULTIPLY form with `>> 3`, not the reference
//     implementation's bit-test accumulation. Those differ by up to 1 LSB per
//     sample, and since the predictor is fed back the error diverges without
//     bound. This form matches ffmpeg's adpcm_ima_expand_nibble.

#ifndef IMA_H
#define IMA_H

#include <stdint.h>

#define IMA_HDR_LEN 4               // int16 predictor, uint8 index, uint8 pad

static const int8_t ima_idx_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8,
                                        -1, -1, -1, -1, 2, 4, 6, 8 };

static const int16_t ima_step_tab[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

typedef struct {
    int32_t pred;
    uint8_t index;
} ima_state_t;

static inline int32_t ima_step(ima_state_t *s, uint8_t nib)
{
    int32_t step = ima_step_tab[s->index];
    int32_t i = (int32_t)s->index + ima_idx_tab[nib];
    if (i < 0) i = 0; else if (i > 88) i = 88;
    s->index = (uint8_t)i;

    int32_t diff = ((2 * (int32_t)(nib & 7u) + 1) * step) >> 3;
    int32_t p = (nib & 8u) ? s->pred - diff : s->pred + diff;
    if (p > 32767) p = 32767; else if (p < -32768) p = -32768;
    s->pred = p;
    return p;
}

#endif
