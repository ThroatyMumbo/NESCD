// knob.h - a panel volume pot on the ADC, driving the same attenuator as 'V'.
//
// Wiring: 10k linear pot, ends to 3V3 and GND, wiper to GP40 (ADC0). GP40..GP46
// are the only free ADC inputs on this board - the ATA bus owns GP2..GP29,
// audio GP30..GP32 and PSRAM GP47, and RP2350B puts ADC0..ADC7 on GP40..GP47.
//
// On at boot, so the pot owns the level from power-up. With no pot fitted the
// input floats near -31 dBFS and `V k` turns it back off.

#ifndef KNOB_H
#define KNOB_H

#include <stdbool.h>
#include <stdint.h>

#define KNOB_PIN     40u            // ADC0
#define KNOB_ADC_IN  0u

// Full clockwise is the loud end. The span is deliberately not the attenuator's
// full 0..-40: everything above about -10 dBFS distorts on this path (see
// CLAUDE.md), so a knob that could reach 0 would waste a quarter of its travel
// and let a careless turn overdrive the NES aux in.
#define KNOB_LOUD_DB   -6
#define KNOB_QUIET_DB  -36

void knob_init(void);
void knob_poll(void);               // ISR-safe; no printf, no float
void knob_enable(bool on);
bool knob_enabled(void);
uint16_t knob_raw(void);            // last smoothed ADC reading, for diagnosis

#endif
