// eject.h - a panel button that works the tray, since the drive's own eject
// button is behind the case.
//
// Wiring: a momentary button from GP33 to GND and nothing else - the internal
// pull-up is the other half. GP33 is the first pin past the audio block, which
// leaves GP41..GP46 free as the board's only remaining ADC inputs.
//
// Unlike knob.c this needs no enable: an unfitted button reads high forever.
// Do not hold it down during 'p 33' - that diagnostic drives the pad, and a
// closed button would short the driver to ground.

#ifndef EJECT_H
#define EJECT_H

#include <stdbool.h>

#define EJECT_PIN     33u
#define EJECT_DEBOUNCE_MS 25

void eject_init(void);

// Toggles the tray on a press: eject when it is shut, load when it is open.
// Returns true when it printed, so the caller can redraw the prompt. Blocks for
// as long as START/STOP UNIT does, so call it only where a command could run.
bool eject_poll(void);

// The debounced press edge alone, touching nothing but the pad. Safe to call
// from inside a stream, where core1 may own the ATA bus.
bool eject_pressed(void);

// The tray half of eject_poll(), for a caller that sampled the edge itself.
bool eject_toggle(void);

#endif
