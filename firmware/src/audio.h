// audio.h - 16-bit stereo I2S out to a PCM5102 DAC, DMA ping-pong fed.
//
// The DAC's line out goes to the NES aux in (expansion port pin 3), and that is
// the only output path; it is heard via the console's composite AV out.
//
// Wiring (PGA2350 -> PCM5102 breakout):
//   GP30 -> LRCK, GP31 -> BCK, GP32 -> DIN, SCK -> GND, VIN -> 3V3, GND -> GND
//
// SCK must be grounded (or its solder bridge closed): that tells the PCM5102A
// to synthesize its system clock from BCK with the internal PLL. Left floating
// the DAC stays silent.

#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"

// GP2..GP29 is the ATA bus and GP47 is the PSRAM chip select, so audio takes
// the next free block up. LRCK and BCK are one side-set pair and must stay
// adjacent; DIN is the OUT pin.
#define AUDIO_LRCK_PIN   30u          // BCK on AUDIO_LRCK_PIN + 1
#define AUDIO_DIN_PIN    32u
#define AUDIO_GPIO_BASE  AUDIO_LRCK_PIN
#define AUDIO_GPIO_COUNT (AUDIO_DIN_PIN - AUDIO_LRCK_PIN + 1u)
#define AUDIO_PIN_MASK   ((1ull << AUDIO_DIN_PIN) | (3ull << AUDIO_LRCK_PIN))

#define AUDIO_SAMPLE_RATE 44100u
#define AUDIO_BUF_FRAMES  512u        // stereo frames per buffer (~11.6 ms)

// Fills n stereo frames. The PIO shifts the high half-word first, so a frame
// is (right << 16) | (uint16_t)left.
typedef void (*audio_fill_fn)(uint32_t *frames, uint n);

// Claim a PIO state machine and two DMA channels; nothing is clocked out yet.
// Call after ata_bus_init() so the ATA program gets the GPIOBASE 0 instance.
bool audio_init(void);

// Start clocking fill()'s output to the DAC. fill() runs in the DMA IRQ.
bool audio_start(audio_fill_fn fill);
void audio_stop(void);

bool     audio_is_playing(void);
bool     audio_fill_is(audio_fill_fn fn);  // playing, and fn owns the DAC
uint64_t audio_frames_out(void);   // frames handed to the DAC since the start
int64_t  audio_elapsed_us(void);
uint     audio_pio_index(void);    // which PIO instance took the GPIOBASE 16 slot

// Master output attenuation, 0 (unity) to -AUDIO_ATTEN_MAX dB, applied to every
// source in the DMA IRQ. The PCM5102A's ~2.1 Vrms full scale is ~16 dB hot into
// a consumer aux input; this is the knob for that, since 4-bit content sits
// ~60 dB above the DAC's own floor and cannot hear the lost headroom.
#define AUDIO_ATTEN_MAX 40
#define AUDIO_ATTEN_MUTE (-(AUDIO_ATTEN_MAX + 1))  // any level below -MAX mutes

// Where audio_init() leaves it. Unity blows out the NES aux in on every source,
// so it is never the right level to come up at; -18 is the measured optimum,
// where chain distortion and chain SNR each sit 8 dB below the content's own
// codec floor.
#define AUDIO_ATTEN_BOOT_DB (-18)

// Q16 fade ramps the fills apply per sample: the PCM5102 thumps on a DC step,
// so every start ramps in and every stop ramps out before the clock dies.
#define AUDIO_FADE_STEP   1024u     // ~1.5 ms in at 44 kHz, kills the click
#define AUDIO_FADE_OUT    128u      // ~12 ms out
#define AUDIO_FADE_OUT_MS 20u

void audio_set_atten_db(int db);      // ISR-safe: a table, not powf
int  audio_get_atten_db(void);

// Called once per buffer from the DMA IRQ, i.e. ~86 Hz and independent of what
// core0 is doing. The knob hangs off this because a stream owns core0 for
// minutes at a time.
void audio_set_tick_hook(void (*fn)(void));

// The PIO clock divider as 16.8 fixed point, i.e. int * 256 + frac. One LSB is
// ~74 ppm at 44.1 kHz.
//
// Setting it is a single 32-bit store: it does not touch the fractional
// accumulator, the FIFO or the state machine, so no BCK edge is lost and the
// PCM5102's BCK-derived PLL sees nothing it does not already see from the
// divider's own dither. Never follow it with pio_sm_clkdiv_restart().
void     audio_set_clkdiv_x256(uint32_t d);
uint32_t audio_get_clkdiv_x256(void);

// The divider that makes the DAC run at `uhz` microhertz, from the live
// clk_sys rather than a literal.
uint32_t audio_clkdiv_x256_for_uhz(uint64_t uhz);

#endif
