// disc.h - the drive itself: sector reads with the retry policy every reader
// here shares, the spin-up/speed dance, and the PSRAM audio slots a producer
// fills for the DAC.
#ifndef DISC_H
#define DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdcore.h"

#define DISC_SECTOR 2048u

// 4x is 704 kB/s, many times what a music track needs. The drive is a 1990s
// unit and there is nothing to gain from spinning it at its maximum.
#define DISC_READ_SPEED_X 4u

// A drive that has not settled fails a read with 5/64/00 illegal-request as
// readily as with a medium error, and both clear on a retry.
#define DISC_READ_TRIES 4

// nsec sectors at lba, retried. Sets disc_fail_* when the retries run out.
int disc_read_sectors(uint32_t lba, uint32_t nsec, void *buf);

// Set when a read exhausts its retries: the LBA that died and the tries spent.
extern uint32_t disc_fail_lba, disc_fail_tries;

// Every attempt's sense, packed key/asc/ascq, oldest first. A settling drive
// and a bad surface both end at 3/02/01; only the earlier attempts tell them
// apart. 0xFFFFFFFF means the read failed without a sense to read.
extern uint32_t disc_fail_sense[DISC_READ_TRIES];

// Spin the drive up and set the read speed once per disc. SET CD SPEED opens a
// seconds-long 5/64 window, so it is not repeated, and with disc_pause_ms set
// the window is waited out by polling `probe` on lba - a one-sector read of
// the kind the caller is about to issue, since READ(10) never succeeds on an
// audio track. NULL probes with READ(10).
typedef bool (*disc_probe_fn)(uint32_t lba);
int  disc_prepare_at(uint32_t lba, disc_probe_fn probe);
int  disc_prepare(void);            // prepare_at(0, NULL)
bool disc_probe_read10(uint32_t lba);
void disc_speed_reset(void);        // the medium changed: set it again
extern uint32_t disc_settle_tries;  // reads the last SET CD SPEED cost

// The producer's 32 KiB SRAM read buffer, borrowed by the other readers that
// run while the producer is parked. Only one of them ever runs at a time.
#define DISC_CHUNK_SECTORS 16u
uint8_t *disc_chunk_buf(void);

// A producer holding its depth owns the drive, and nothing else may poll the
// tray, so it asks itself: false once TEST UNIT READY answers 2/3A. Call it
// from every wait long enough to hide a tray opening.
#define DISC_IDLE_TUR_MS 2000u
bool disc_heartbeat(uint32_t *parked_at);

extern uint32_t (*disc_now_ms)(void);
extern void     (*disc_idle)(void);        // run while waiting
extern void     (*disc_pause_ms)(uint32_t ms);   // core0 only; NULL off-target

// -- what every producer shares ---------------------------------------------

// The consumer's cursor, read on the producer core; NULL holds at start_seq.
extern uint32_t (*disc_consumer)(void);

// Hold `depth` blocks ahead of the consumer: a steady trickle rather than a
// fill and a long idle, which makes the drive park and re-seek. False once
// *stop_req is set, or with *gone_flag set once the tray poll says 2/3A.
bool disc_wait_depth(uint32_t seq, uint32_t start_seq, uint32_t depth,
                     volatile uint32_t *stop_req, volatile uint32_t *gone_flag);

// One struct for whichever producer ran last, so S and the first-chunk waits
// read the same counters.
typedef struct {
    volatile uint32_t running, chunks, retries, blocks, laps, worst_read_ms;
    volatile uint32_t last_sense;              // packed key/asc/ascq
    volatile int32_t  last_rc;
} disc_stat_t;

extern disc_stat_t disc_stat;

void disc_stat_reset(void);
void disc_stat_read_failed(int arc);         // packs the sense of a failed read
uint32_t disc_stat_read_ms(uint32_t t0);     // ms since t0, folded into worst_read_ms

// -- audio slots -----------------------------------------------------------
//
// Blocks live in a fixed-slot array indexed seq % slots and are overwritten as
// the producer laps them. The tags stay in SRAM, so nothing in PSRAM is ever a
// synchronization variable.

#define DISC_SLOTS_MAX 2048u

// pcm16 is the widest the mastering tools emit (2940 B); the cap keeps a
// corrupt header from sizing the slot array out of the region.
#define DISC_BLOCK_MAX 4096u

// Where the blocks live. Sized in bytes; the slot count falls out of the
// item's block width. Call before the producer runs.
void disc_slots_attach(uint8_t *base, uint32_t bytes);

// Size the slots for block_bytes, clear every tag, refuse fewer than
// min_slots. The width is one value for the whole array, so no consumer may be
// live across this.
int  disc_slots_open(uint32_t block_bytes, uint32_t min_slots);

uint32_t disc_slot_count(void);

// One whole block into its slot: tag cleared first, published with release.
void disc_slot_put(uint32_t seq, const uint8_t *block);

// The block for `seq`, or false if it is not currently in a slot. Safe from an
// IRQ on the consumer core: it reads an SRAM tag, never the producer's cursors.
bool disc_slot_get(uint32_t seq, const uint8_t **p, uint32_t *n);

#endif
