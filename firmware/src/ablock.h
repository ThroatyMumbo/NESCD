// ablock.h - incremental audio-block cursor shared by the DMA-IRQ fills.
// Pico-free; always_inline keeps it inside the RAM-resident callers.

#ifndef ABLOCK_H
#define ABLOCK_H

#include <stdint.h>

#include "audiofmt.h"
#include "ima.h"

typedef struct {
    const uint8_t *p; uint32_t left;
    const uint8_t *q; uint32_t qlen;    // second run: a disc block can wrap
    ima_state_t ima;
    uint8_t  nib_hi;                    // high nibble held back from the last byte
    uint32_t off;                       // sample within the block
} ablock_t;

static inline __attribute__((always_inline)) uint8_t ablock_take(ablock_t *ab)
{
    if (ab->left) { ab->left--; return *ab->p++; }
    if (ab->qlen) { ab->qlen--; return *ab->q++; }
    return 0;
}

// Point the cursor at a block's runs and eat the ADPCM state header.
static inline __attribute__((always_inline)) void
ablock_open(ablock_t *ab, const uint8_t *a, uint32_t na,
            const uint8_t *b, uint32_t nb, uint32_t fmt)
{
    ab->p = a; ab->left = na;
    ab->q = b; ab->qlen = nb;
    if (fmt == AFMT_ADPCM4) {
        uint8_t lo = ablock_take(ab), hi = ablock_take(ab);
        ab->ima.pred = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        ab->ima.index = ablock_take(ab);
        if (ab->ima.index > 88) ab->ima.index = 88;
        (void)ablock_take(ab);                   // reserved
    }
}

static inline __attribute__((always_inline)) int32_t
ablock_next(ablock_t *ab, uint32_t fmt)
{
    if (fmt == AFMT_PCM16) {
        uint8_t lo = ablock_take(ab), hi = ablock_take(ab);
        return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    }
    if (fmt == AFMT_CDDA) {                      // mono callers get the left channel
        uint8_t lo = ablock_take(ab), hi = ablock_take(ab);
        (void)ablock_take(ab); (void)ablock_take(ab);
        return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    }
    uint8_t nib;
    if (ab->off & 1u) {
        nib = ab->nib_hi;
    } else {
        uint8_t b = ablock_take(ab);
        ab->nib_hi = (uint8_t)(b >> 4);
        nib = (uint8_t)(b & 0x0Fu);
    }
    return ima_step(&ab->ima, nib);
}

// One stereo frame of an AFMT_CDDA block.
static inline __attribute__((always_inline)) void
ablock_next2(ablock_t *ab, int32_t *l, int32_t *r)
{
    uint8_t lo = ablock_take(ab), hi = ablock_take(ab);
    *l = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    lo = ablock_take(ab); hi = ablock_take(ab);
    *r = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

#endif
