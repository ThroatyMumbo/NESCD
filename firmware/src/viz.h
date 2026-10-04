// viz.h - a spectrum for the CD player ROM's bars: the DAC fill taps every
// sample it plays into a ring, the idle loop turns the latest window into bands.
// Pico-free, so test/host_viz.c runs the same code over a WAV.
#ifndef VIZ_H
#define VIZ_H

#include <stdint.h>

#define VIZ_BANDS 8
#define VIZ_N     1024u              // FFT window, 23 ms at 44.1 kHz

extern int16_t viz_ring[VIZ_N];
extern volatile uint32_t viz_w;

// Called from the DMA IRQ once per stereo frame it plays.
static inline void viz_tap(int32_t l, int32_t r)
{
    uint32_t w = viz_w;
    viz_ring[w & (VIZ_N - 1u)] = (int16_t)((l + r) >> 1);
    viz_w = w + 1u;
}

// The last VIZ_N samples as VIZ_BANDS levels, 0 silent to 255 loud.
void viz_bands(uint8_t out[VIZ_BANDS]);

#endif
