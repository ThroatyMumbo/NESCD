// tone.c - full-scale sine source for the DAC, built once into a table.

#include "tone.h"

#include <math.h>

#include "pico/stdlib.h"

#include "audio.h"

#define TBITS 12
#define TSIZE (1u << TBITS)           // phase quantization floor ~-78 dBc

static int16_t  tab[TSIZE];
static bool     built;
static uint32_t phase, inc, hz_now;

void tone_reset(uint32_t hz)
{
    if (!built) {
        for (uint32_t i = 0; i < TSIZE; i++)
            tab[i] = (int16_t)lrintf(32767.0f
                                     * sinf(2.0f * (float)M_PI * i / TSIZE));
        built = true;
    }
    if (!hz) hz = 1000;
    hz_now = hz;
    phase = 0;
    inc = (uint32_t)(((uint64_t)hz << 32) / AUDIO_SAMPLE_RATE);
}

uint32_t tone_hz(void) { return hz_now; }

void __not_in_flash_func(tone_fill)(uint32_t *out, uint n)
{
    for (uint i = 0; i < n; i++) {
        uint16_t s = (uint16_t)tab[phase >> (32 - TBITS)];
        out[i] = ((uint32_t)s << 16) | s;
        phase += inc;
    }
}
