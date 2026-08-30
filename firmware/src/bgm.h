// bgm.h - background music during gameplay: the disc's PSRAM audio slots
// behind one block() hook, free-running at 44100 Hz.

#ifndef BGM_H
#define BGM_H

#include <stdbool.h>
#include <stdint.h>

#include "cdcore.h"

// block() runs IN THE DMA IRQ (no blocking, no printf); false plays as that
// block's silence. The cursor is monotonic: block() folds it with bgmloop.h.
typedef struct {
    bool (*block)(uint32_t seq, cd_span_t *r);
    uint32_t fmt, spr, blk, nblocks;
    bool     loop;
    uint32_t loop_start, loop_end;  // samples, bgmloop.h; 0/0 = the whole array
    const char *name;
} bgm_source_t;

// The game's music byte ($1FF9): bit 7 holds track N in place.
#define BGM_HOLD 0x80u

// Play `s` from block start_seq. `key` names the track for bgm_track() and
// the resume rule; the source struct is copied, so it may be transient.
int  bgm_play_src(const bgm_source_t *s, uint32_t key, uint32_t start_seq);

// The disc track track_open() holds, whose producer is already running from
// start_seq.
int  bgm_play_disc(uint32_t key, uint32_t start_seq);

// The open audio disc from an LBA to the end of its audio run, stereo; `key`
// is the track number. The cdda producer must already be running past it.
int  bgm_play_cdda(uint32_t key, uint32_t start_lba);

// Where a resume of `key` would start: the saved cursor when it is the track
// that played last and has not played out, else 0.
uint32_t bgm_resume_seq(uint32_t key);

// The consumer's cursor, for the disc producer's depth hold.
uint32_t bgm_cursor(void);

// Fade out and release the DAC, keeping the cursor for a resume. A no-op when
// bgm does not own the DAC, so every stream path may call it blindly.
void bgm_stop(void);

// Stop if playing and drop the resume point: the next request starts from the
// top. An explicit "music off" forgets; a stream's own fade does not.
void bgm_forget(void);

// The game's "music off": a loop stops and forgets, a one-shot plays out to
// its end (its own end releases the DAC) and is forgotten meanwhile.
void bgm_release(void);

// Freeze in place (fade out, keep the cursor) or release (fade back in).
void bgm_hold(bool on);

bool     bgm_active(void);
uint32_t bgm_track(void);           // 0 unless actually playing
bool     bgm_ended(void);           // a non-looping track played out
void     bgm_report(void);

#endif
