// ata.h - Parallel ATA taskfile host for the RP2350B (PGA2350).
//
// Role: HOST. This MCU is the IDE controller and drives any ATAPI CD-ROM
// (developed against a Pacific Digital MACH 52). The PACKET layer is atapi.c.
//
// The RP2350 is 5V tolerant, so the 40-pin ATA bus (5V TTL) connects DIRECTLY
// to GPIO - no level shifters, no transceivers.
//
// 40-pin ATA cable. Pin 20 is the key (no contact); tie every ground:
//   GND     pins 2,19,22,24,26,30,40
//   unused  pin 28 CSEL, 32 IOCS16#, 34 PDIAG#, 39 DASP#
//
// Jumper the drive to MASTER: it is the only device on the bus.

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pico/stdlib.h"

// ---- GPIO assignment (RP2350B / PGA2350) --------------------------------
// Reserved on this board: GP0/GP1 = UART0 console (debugprobe), GP47 = PSRAM CS.
// The bus is one contiguous block, GP2..GP29: inside a PIO GPIOBASE=0 window and
// below GP31, so gpio_get_all() and gpio_put_masked() still move the whole bus
// in one instruction.
#define PIN_DD0    2u          // DD0..DD15 = GPIO2..GPIO17 (bidirectional)
#define DD_MASK    (0xFFFFu << PIN_DD0)
#define PIN_DA0    18u         // out: register select bit 0   (ATA pin 35)
#define PIN_DA1    19u         // out: register select bit 1   (ATA pin 33)
#define PIN_DA2    20u         // out: register select bit 2   (ATA pin 36)
#define PIN_CS0    21u         // out: /CS0 command block      (ATA pin 37)
#define PIN_CS1    22u         // out: /CS1 control block      (ATA pin 38)
#define PIN_DIOR   23u         // out: /DIOR read strobe       (ATA pin 25)
#define PIN_DIOW   24u         // out: /DIOW write strobe      (ATA pin 23)
#define PIN_RESET  25u         // out: /RESET                  (ATA pin 1)
#define PIN_INTRQ  26u         // in : INTRQ                   (ATA pin 31)
#define PIN_IORDY  27u         // in : IORDY                   (ATA pin 27)
#define PIN_DMARQ  28u         // in : DMARQ                   (ATA pin 21)
#define PIN_DMACK  29u         // out: /DMACK                  (ATA pin 29)

// DA0..2 + both chip selects move together in one masked store.
#define ATA_ADDR_MASK (0x1Fu << PIN_DA0)

// DD0..DD15 interleave across the connector's two rows. Verify each line while
// wiring: ATA 17,15,13,11,9,7,5,3 = DD0..DD7, ATA 4,6,8,10,12,14,16,18 = DD8..DD15.

// ---- register blocks ----------------------------------------------------
#define ATA_CS_CMD 0u          // assert /CS0: command block
#define ATA_CS_CTL 1u          // assert /CS1: control block

// Command block (ATA_CS_CMD), selected by DA2:0.
#define ATA_REG_DATA      0u   // 16-bit
#define ATA_REG_ERROR     1u   // read
#define ATA_REG_FEATURES  1u   // write
#define ATA_REG_INTREASON 2u   // read  (ATAPI)
#define ATA_REG_SECCOUNT  2u   // write
#define ATA_REG_LBA_LOW   3u
#define ATA_REG_BCOUNT_LO 4u   // byte count low  (ATAPI)
#define ATA_REG_BCOUNT_HI 5u   // byte count high (ATAPI)
#define ATA_REG_DEVICE    6u   // DEV = bit 4
#define ATA_REG_STATUS    7u   // read
#define ATA_REG_COMMAND   7u   // write

// Control block (ATA_CS_CTL), DA=6.
#define ATA_REG_ALTSTATUS 6u   // read  (does not clear INTRQ)
#define ATA_REG_DEVCTL    6u   // write

// ---- status / device control bits ---------------------------------------
#define ATA_ST_BSY   0x80u
#define ATA_ST_DRDY  0x40u
#define ATA_ST_DF    0x20u
#define ATA_ST_DSC   0x10u
#define ATA_ST_DRQ   0x08u
#define ATA_ST_ERR   0x01u

#define ATA_DEVCTL_SRST 0x04u
#define ATA_DEVCTL_NIEN 0x02u

// ---- commands -----------------------------------------------------------
#define ATA_CMD_DEVICE_RESET    0x08u
#define ATA_CMD_PACKET          0xA0u
#define ATA_CMD_IDENTIFY_PACKET 0xA1u
#define ATA_CMD_SET_FEATURES    0xEFu
#define ATA_FEAT_XFER_MODE      0x03u   // SECCOUNT = 0x00|mode for PIO flow

// ---- bus timing ---------------------------------------------------------
// Two PIO state machines own DD0..DD15 plus one strobe each; the CPU keeps
// DA0..2, CS0#, CS1#, RESET# and DMACK#. Timing is an ATA PIO mode rather
// than one uniform phase, so t1 setup, t2 pulse width and the recovery to t0
// are separate. Modes 3 and 4 are defined around IORDY flow control, which
// this driver does not implement, so only 0..2 are accepted.
#define ATA_PIO_MODE_MAX 2u

// Select a mode. Returns the achieved read cycle in ns, which is >= the mode
// minimum: the solver lengthens t0 rather than shortening any phase.
uint32_t ata_set_pio_mode(uint mode);

// Tell the drive which PIO mode it is about to be clocked at, before the host
// retimes. A rejection is survivable: mode 0 is the power-on default.
bool     ata_set_xfer_mode(uint mode);
uint     ata_get_pio_mode(void);
uint32_t ata_get_cycle_ns(void);
void     ata_timing_report(void);

// Bus occupancy, for the b benchmark's bus/wall split. Reset by the caller.
extern uint64_t ata_stat_words;
extern uint64_t ata_stat_bus_us;

// Called from inside ata_wait_not_bsy()/ata_wait_drq() while they spin, so a
// long ATA command does not starve whatever else the CPU owes time to.
// main() points this at usb_link_pump().
extern void (*ata_wait_hook)(void);

// Bring the pins up under SIO at their idle levels, then hand the data bus
// and both strobes to PIO and select Mode 0. False means no free state
// machines or program space, and the bus is left inert.
bool     ata_bus_init(void);

// SIO idle levels only. ata_bus_init() calls it first; diag.c calls it when
// putting the bus back after driving pins itself.
void     ata_gpio_init(void);
void     ata_hard_reset(void);
bool     ata_soft_reset(uint32_t timeout_ms);

uint8_t  ata_reg_read8(uint cs, uint da);
void     ata_reg_write8(uint cs, uint da, uint8_t v);
uint8_t  ata_status(void);
uint8_t  ata_altstatus(void);

// Burst transfers hold the address static and strobe only /DIOR or /DIOW.
// Any word count is accepted; an odd one costs no extra bus cycle.
void     ata_read_data_burst(uint16_t *dst, size_t words);
void     ata_write_data_burst(const uint16_t *src, size_t words);

// Strobe /DIOR the given number of times and throw the data away. The drive
// announces a burst size the host may not have room for, and draining it a
// word at a time through ata_read_data_burst() costs several times as much.
void     ata_skip_data(size_t words);

// DMA-backed read that returns immediately, so the caller can do something
// useful while the bus runs. begin() returns false when the fast path does
// not apply - an unaligned destination, an odd count, or a read already in
// flight - and the caller should fall back to ata_read_data_burst().
bool     ata_read_begin(uint16_t *dst, size_t words);
bool     ata_read_poll(void);
void     ata_read_end(void);

// Hand every ATA pin back to SIO at its idle level, and take it back again.
// diag.c drives pins directly and needs the bus for the duration.
void     ata_bus_release(void);
void     ata_bus_take(void);

// Force both strobes high and restart both machines. A disabled state machine
// holds whatever the PIO output register last latched, so lifting a stuck
// strobe means writing that register, not just stopping the machine.
void     ata_bus_recover(void);

bool     ata_wait_not_bsy(uint32_t timeout_ms);
bool     ata_wait_drq(uint32_t timeout_ms);
void     ata_select_device(uint dev);

// Wait for the drive to post its ATAPI signature after a reset. Status is
// useless here: a packet device idles at 0x00, and a floating bus reads 0x7F
// because DD7 alone carries no pull-up.
bool     ata_wait_signature(uint32_t timeout_ms);

// Post-reset signature: mid 0x14 / high 0xEB marks an ATAPI packet device.
void     ata_read_signature(uint8_t *mid, uint8_t *high);

// IDENTIFY PACKET DEVICE (0xA1) into 256 words. A packet device aborts the
// plain IDENTIFY DEVICE (0xEC), so this is the one that works on a CD-ROM.
bool     ata_identify_packet(uint16_t *id256);

// Extract a byte-swapped ASCII field from an IDENTIFY buffer (model = words
// 27..46, firmware = 23..26, serial = 10..19), trimming trailing spaces.
void     ata_id_string(const uint16_t *id, int first, int nwords,
                       char *out, int outsz);

#endif
