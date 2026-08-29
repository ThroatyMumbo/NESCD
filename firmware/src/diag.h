// diag.h - GPIO-level wiring diagnostics for the ATA harness.
//
// These run without the drive responding to anything, so they are the tools for
// "nothing works at all". Every report names the GPIO and the ATA pin.

#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>
#include "pico/stdlib.h"

// Snapshot every mapped line: level, direction and ATA pin number.
void diag_snapshot(void);

// Classify each line by whether it follows an internal pull. FLOAT means
// nothing is driving it; DRIVEN means the far end holds it regardless.
void diag_float_scan(void);

// Drive each host-driven line high in turn and look for any other line that
// follows it. Catches shorts between adjacent wires and crimps.
void diag_short_scan(void);

// Square wave on one GPIO for a few seconds so it can be traced with a meter
// at the connector. Leaves the bus back in its idle state.
void diag_blink(uint gpio, uint32_t seconds);

// Print every line's state whenever any of them changes, until a key arrives.
void diag_monitor(void);

#endif
