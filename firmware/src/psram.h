// psram.h - APS6404 PSRAM on the RP2350 QMI CS1 window (GP47 on the PGA2350).
// Once psram_init() returns non-zero the chip is ordinary read/write memory at
// PSRAM_XIP_BASE.
#ifndef PSRAM_H
#define PSRAM_H

#include <stddef.h>
#include "pico/types.h"

#define PSRAM_CS_PIN   47u          // PIMORONI_PGA2350_PSRAM_CS_PIN (board header)
#define PSRAM_XIP_BASE 0x11000000u  // QMI CS1 memory-mapped window (M1)

// The same bytes with the XIP cache bypassed. hardware/address_mapped.h only
// defines xip_nocache_noalloc_alias() for RP2040, so the offset is spelt out.
#define PSRAM_NOCACHE_BASE (PSRAM_XIP_BASE + 0x04000000u)

// XIP maintenance offsets are relative to XIP_BASE, not to the CS1 window.
#define PSRAM_CACHE_OFF (PSRAM_XIP_BASE - 0x10000000u)

// Bring up the PSRAM on QMI chip-select 1. Returns the detected size in BYTES,
// or 0 if no APS6404 answered (KGD != 0x5D). MUST be called with clk_sys at its
// final frequency: the M1 timing is derived from clock_get_hz(clk_sys).
size_t psram_init(uint cs_pin);

// Detected size, and the divisor and rxdelay psram_init() programmed. Valid
// after psram_init(); psram_bytes is 0 if nothing answered.
extern size_t   psram_bytes;
extern uint32_t psram_divisor, psram_rxdelay;

#endif // PSRAM_H
