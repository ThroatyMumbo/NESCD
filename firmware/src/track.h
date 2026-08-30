// track.h - a music track streamed off the disc: core1 reads a CDTRACK item's
// block array into the PSRAM audio slots, bgm.c plays them out.
#ifndef TRACK_H
#define TRACK_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "cdcore.h"
#include "disctask.h"

#define TRACK_MAGIC  "CDTRACK"      // 8 bytes with its NUL
#define TRACK_VER    2u
#define TRACK_F_LOOP 1u             // in `flags`

// The item's header, little-endian and naturally aligned; tools/mktrack.py is
// the other end of it. The words a pure audio track has no use for are named
// rather than reserved, and left at 0.
typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t nframes;               // audio blocks
    uint32_t unused_slices;
    uint32_t unused_fields;
    uint32_t nrecords;              // 0
    uint32_t table_off;             // sizeof(*h)
    uint32_t payload_off;           // sizeof(*h) + 4: the empty sentinel table
    uint32_t payload_bytes;         // 0
    uint32_t crc32;
    uint32_t unused_palette;
    uint32_t flags;                 // TRACK_F_LOOP
    uint32_t audio_format;          // audiofmt.h AFMT_*
    uint32_t audio_rate;            // mastering rate, informational
    uint32_t audio_channels;        // 1: the destination is expansion pin 3
    uint32_t audio_spr;             // samples per block
    uint32_t audio_block;           // bytes per block
    uint32_t audio_off;             // TRACK_DATA
    uint32_t audio_bytes;           // nframes * audio_block, exactly
    uint32_t audio_crc32;
    uint32_t loop_start;            // samples, block-aligned; 0/0 = whole array
    uint32_t loop_end;              // samples, exact
    uint32_t lead_in;               // silence the masterer prepended
} trk_hdr_t;

// On-disc, and check_hdr keys table_off/payload_off off it.
static_assert(sizeof(trk_hdr_t) == 96, "CDTRACK header is 96 bytes on disc");

typedef struct {
    uint32_t lba, sectors;           // the item
    uint32_t nblocks, blk, fmt, spr;
    bool     loop;
    uint32_t loop_start, loop_end;   // samples, bgmloop.h; 0/0 = the whole array
} track_t;

// Blocks the producer runs ahead of the consumer: ~17 s of adpcm, 370 KiB.
#define TRACK_DEPTH     512u

// The block array starts at byte 100 of the item, so blocks straddle sectors
// wherever the producer starts and stops.
#define TRACK_DATA      100u

// One chunk of blocks past the depth, plus margin, or a slot could be lapped.
#define TRACK_SLOTS_MIN (TRACK_DEPTH + 64u)

// Read and check the item's header; sizes the slots for its block width.
int  track_open(uint32_t lba, uint32_t sectors);
bool track_is_open(void);
const track_t *track_info(void);

// Producer, core1. Slots are tagged with a MONOTONIC seq (the disc index is
// bgmloop.h's fold), so a lap seam can never alias an unplayed block's slot.
// Counters land in disc_stat; the consumer's cursor is disc_consumer.
int  track_fill(uint32_t start_seq);
void track_stop(void);
void track_reset(void);
uint32_t track_next_seq(void);       // the producer's cursor
bool track_eos(void);                // a non-looping track read out
bool track_medium_gone(void);

extern const disc_job_t track_job;   // the three above, for disctask_run()

#endif
