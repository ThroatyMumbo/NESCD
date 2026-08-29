// tone.h - full-scale sine into the I2S DAC, for calibrating the analog leg.
//
// The jingle is the wrong instrument for a level sweep: its 25% duty pulse
// carries ~-0.5 FS of DC that every coupling cap downstream strips, so its
// analog peak is ~1.5 dB adrift of its digital one and depends on the corner
// frequency. This peaks at digital full scale with no DC and one spectral line,
// which is what a clipping threshold and a THD figure both need.

#ifndef TONE_H
#define TONE_H

#include <stdint.h>
#include "pico/types.h"

void tone_reset(uint32_t hz);
void tone_fill(uint32_t *frames, uint n);
uint32_t tone_hz(void);

#endif
