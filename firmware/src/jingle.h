// jingle.h - the two-voice chiptune loop, as an audio_fill_fn.
//
// The bring-up signal source for the I2S path: bgm.c's disc player replaces it
// behind the same callback.

#ifndef JINGLE_H
#define JINGLE_H

#include <stdint.h>
#include "pico/types.h"

// Rewind the score and silence both voices. Call before audio_start().
void jingle_reset(void);

// audio_fill_fn: n stereo frames, high half-word right, low half-word left.
void jingle_fill(uint32_t *out, uint n);

#endif
