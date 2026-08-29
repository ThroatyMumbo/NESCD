// jingle.c - a 25% pulse lead and a 16-step triangle bass over a 32-step loop,
// rendered straight into the I2S buffers.

#include "jingle.h"

#include <math.h>

#include "audio.h"

#define BPM        140
#define STEPS      32       // sixteenth notes in the loop
#define HOLD       (-1)     // keep the note that is already sounding
#define REST       0

// MIDI note numbers. The loop is a C major fanfare that falls back home.
static const int8_t lead_track[STEPS] = {
    72, HOLD, 76, HOLD, 79, HOLD, 84, HOLD,     // C5  E5  G5  C6
    83, 84,   86, HOLD, 88, HOLD, HOLD, HOLD,   // B5  C6  D6  E6
    84, HOLD, 81, HOLD, 77, HOLD, 79, HOLD,     // C6  A5  F5  G5
    76, HOLD, 72, HOLD, REST, HOLD, HOLD, HOLD, // E5  C5  ...
};

static const int8_t bass_track[STEPS] = {
    48, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD,   // C3
    55, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD,   // G3
    53, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD, HOLD,   // F3
    55, HOLD, HOLD, HOLD, 48,   HOLD, HOLD, HOLD,   // G3 C3
};

// Per-sample amplitude multipliers in 16.16, giving each voice its own decay.
#define LEAD_DECAY 65515    // plucky
#define BASS_DECAY 65530    // nearly sustained

#define LEAD_LEVEL 11000    // peak amplitude before mixing
#define BASS_LEVEL 9000

static uint32_t note_inc[128];      // phase increment per MIDI note
static bool     table_built;

static struct {
    uint32_t phase, inc;
    uint32_t amp;                   // 16.16, decays each sample
} lead, bass;

static uint32_t samples_per_step;
static uint32_t step_countdown;
static uint     step_index;

void jingle_reset(void)
{
    if (!table_built) {
        for (int n = 0; n < 128; n++) {
            double hz = 440.0 * pow(2.0, (n - 69) / 12.0);
            note_inc[n] = (uint32_t)(hz * 4294967296.0 / AUDIO_SAMPLE_RATE);
        }
        table_built = true;
    }
    samples_per_step = (AUDIO_SAMPLE_RATE * 60) / (BPM * 4);
    step_countdown = 0;
    step_index = 0;
    lead.phase = lead.inc = lead.amp = 0;
    bass.phase = bass.inc = bass.amp = 0;
}

static void trigger(int8_t note, uint32_t *phase, uint32_t *inc, uint32_t *amp,
                    uint32_t level)
{
    if (note == HOLD) return;
    if (note == REST) {
        *amp = 0;
        return;
    }
    *phase = 0;
    *inc = note_inc[(uint8_t)note];
    *amp = level << 16;
}

static void advance_step(void)
{
    trigger(lead_track[step_index], &lead.phase, &lead.inc, &lead.amp, LEAD_LEVEL);
    trigger(bass_track[step_index], &bass.phase, &bass.inc, &bass.amp, BASS_LEVEL);
    step_index = (step_index + 1) % STEPS;
}

// Envelope step. amp is 16.16, so a plain 32-bit multiply by the decay
// factor would overflow long before the note fades; widen it.
static inline uint32_t decay(uint32_t amp, uint32_t factor)
{
    return (uint32_t)(((uint64_t)amp * factor) >> 16);
}

// 25% duty pulse, the classic NES lead timbre.
static inline int32_t pulse25(uint32_t phase)
{
    return (phase >> 30) == 0 ? 32767 : -32768;
}

// Triangle quantized to 16 steps, like the NES triangle channel.
static inline int32_t triangle16(uint32_t phase)
{
    int32_t t = (int32_t)(phase >> 27);         // 0..31
    if (t >= 16) t = 31 - t;                    // 0..15..0
    return (2 * t - 15) * 2184;                 // -32760..32760, no DC offset
}

void jingle_fill(uint32_t *out, uint n)
{
    for (uint i = 0; i < n; i++) {
        if (step_countdown == 0) {
            advance_step();
            step_countdown = samples_per_step;
        }
        step_countdown--;

        // Scale each full-range waveform by its envelope, then sum. The shift
        // has to come after the multiply or the waveform quantizes to 0/-1.
        int32_t sample = 0;
        sample += (pulse25(lead.phase)    * (int32_t)(lead.amp >> 16)) >> 15;
        sample += (triangle16(bass.phase) * (int32_t)(bass.amp >> 16)) >> 15;

        lead.phase += lead.inc;
        bass.phase += bass.inc;
        lead.amp = decay(lead.amp, LEAD_DECAY);
        bass.amp = decay(bass.amp, BASS_DECAY);

        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        // Mono jingle sent to both channels.
        uint16_t s = (uint16_t)(int16_t)sample;
        out[i] = ((uint32_t)s << 16) | s;
    }
}
