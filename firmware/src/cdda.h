// cdda.h - a Red Book audio disc: its table of contents, and the core1
// producer that reads it as raw 2352-byte sectors into the PSRAM slots, one
// sector per block. Blocks are tagged with the absolute LBA, so the consumer
// walks across track boundaries with no seek and the track is a lookup.
#ifndef CDDA_H
#define CDDA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdcore.h"
#include "disctask.h"

#define CDDA_TRACKS 100u             // indexed by track number, 1..99

typedef struct {
    uint8_t  first, last;
    uint32_t start[CDDA_TRACKS];     // each track's first sector
    uint32_t audio_end[CDDA_TRACKS]; // where the audio run from that track stops
    uint32_t leadout;
    uint8_t  ctrl[CDDA_TRACKS];      // TOC control nibble; bit 2 = data
    uint8_t  cap;                    // page 2A byte 5, CDDA_CAP_*; CDDA_CAP_UNKNOWN if unread
} cdda_toc_t;

#define CDDA_CAP_UNKNOWN  0xFFu
#define CDDA_CAP_CMDS     0x01u      // READ CD of audio is supported
#define CDDA_CAP_ACCURATE 0x02u      // no sample jitter between successive reads

// Sectors per READ CD: one burst (<= 0xFFFE B) and inside the chunk buffer,
// so every sector lands 4-aligned for the DMA path.
#define CDDA_CHUNK      13u
#define CDDA_DEPTH      1024u        // sectors ahead of the consumer, ~13.6 s
#define CDDA_SLOTS_MIN  (CDDA_DEPTH + 128u)
#define CDDA_RETRIES    2u
// A failure this close to the run's end is the drive refusing the sectors
// before a lead-out, which many do; the run ends there.
#define CDDA_TAIL       75u
// A data session after the audio sits behind lead-out, lead-in and pregap.
#define CDDA_SESSION_GAP 11400u

// Pure: the READ TOC response into a table. The harness runs it too.
int cdda_parse_toc(const uint8_t *p, size_t got, cdda_toc_t *t);

// READ TOC, parse, read the capabilities page, then prepare the drive at the
// first audio track. CD_ENOAUDIO when the first track is data.
int  cdda_open(void);
void cdda_close(void);
bool cdda_is_open(void);
const cdda_toc_t *cdda_toc(void);

bool     cdda_is_audio(uint8_t track);
uint8_t  cdda_first_audio(void);
uint8_t  cdda_track_of(uint32_t lba);        // 0 outside every track
uint32_t cdda_track_len(uint8_t track);       // sectors, to its audio end
void     cdda_msf(uint32_t sectors, uint8_t *min, uint8_t *sec);

// Whether a producer whose cursor is next_seq still holds sector `at` in a
// ring of `slots`, with two chunks of margin either side for the one it is
// writing now: a play there needs no re-read.
bool cdda_slots_hold(uint32_t next_seq, uint32_t at, uint32_t slots);

// Producer, core1: from an LBA to the audio run's end.
int  cdda_fill(uint32_t start_lba);
void cdda_stop(void);
void cdda_reset(void);
uint32_t cdda_next_seq(void);
bool cdda_eos(void);
bool cdda_medium_gone(void);
uint8_t cdda_read_flags(void);               // the READ CD byte 9 this drive took

extern const disc_job_t cdda_job;

#endif
