// bgm.c - background music while the game renders: the disc's audio slots
// behind one block() hook, free-running at the mastered 44100 Hz with no servo.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ablock.h"
#include "atapi.h"
#include "audio.h"
#include "audiofmt.h"
#include "bgm.h"
#include "bgmloop.h"
#include "cdcore.h"
#include "cdda.h"
#include "disc.h"
#include "track.h"

// Samples from the end at which a non-looping track starts its ramp out, so
// the content's last sample never lands as a DC step.
#define FADE_TAIL (0x10000u / AUDIO_FADE_OUT)

// Snapshotted at play: the caller's struct may be transient.
static bgm_source_t src;
static bool have_src;

static struct {
    volatile uint32_t seq;
    ablock_t ab;
    bgm_loop_t loop;
    uint32_t end_off;               // samples this visit plays (bgmloop.h)
    bool     open;
    bool     hole;                  // block missing: this seq plays as silence
    uint32_t amp;                   // Q16 fade
    volatile bool fading;
    volatile bool hold;             // frozen at the cursor once amp reaches 0
    volatile bool ended;            // the fade ran out: the DAC can be released
    volatile bool done;             // a one-shot played out: nothing to resume
} cur;

static uint32_t track_cur;          // last track played, kept for the resume
static struct { uint32_t blocks, silent, worst_isr_us; } st;

static void __not_in_flash_func(bgm_fill)(uint32_t *out, uint n)
{
    uint32_t t_in = time_us_32();

    for (uint i = 0; i < n; i++) {
        if (cur.ended) { out[i] = 0; continue; }
        if (cur.fading && cur.amp == 0) { cur.ended = true; out[i] = 0; continue; }
        if (cur.hold && cur.amp == 0) { out[i] = 0; continue; }
        if (cur.ab.off >= cur.end_off) {
            cur.seq++;
            cur.ab.off = 0;
            cur.open = cur.hole = false;
            if (!src.loop && cur.seq >= src.nblocks) {
                cur.done = cur.ended = true; out[i] = 0; continue;
            }
        }
        if (!cur.open && !cur.hole) {
            cd_span_t r;
            cur.end_off = bgm_loop_end_off(&cur.loop, cur.seq, src.spr);
            if (src.block(cur.seq, &r)) {
                ablock_open(&cur.ab, r.a, r.na, r.b, r.nb, src.fmt);
                cur.open = true;
                st.blocks++;
            } else {
                cur.hole = true;
                st.silent++;
            }
        }
        if (!src.loop && cur.seq + 1u == src.nblocks &&
            src.spr - cur.ab.off <= FADE_TAIL)
            cur.fading = true;

        int32_t l = 0, r = 0;
        if (!cur.hole) {
            if (src.fmt == AFMT_CDDA) ablock_next2(&cur.ab, &l, &r);
            else l = r = ablock_next(&cur.ab, src.fmt);
        }
        l = (int32_t)(((int64_t)l * (int64_t)cur.amp) >> 16);
        r = (int32_t)(((int64_t)r * (int64_t)cur.amp) >> 16);
        if (cur.fading || cur.hold) {
            cur.amp = cur.amp > AUDIO_FADE_OUT ? cur.amp - AUDIO_FADE_OUT : 0u;
        } else if (cur.amp < 0xFFFFu) {
            cur.amp += AUDIO_FADE_STEP;
            if (cur.amp > 0xFFFFu) cur.amp = 0xFFFFu;
        }
        // The high half-word is the right slot (audio_i2s.pio).
        out[i] = ((uint32_t)(uint16_t)(int16_t)r << 16) | (uint16_t)(int16_t)l;
        cur.ab.off++;
    }

    uint32_t took = time_us_32() - t_in;
    if (took > st.worst_isr_us) st.worst_isr_us = took;
}

uint32_t bgm_resume_seq(uint32_t key)
{
    if (key != track_cur || !have_src || cur.done) return 0;
    return cur.seq;
}

uint32_t bgm_cursor(void) { return cur.seq; }

static uint32_t ms_of(uint32_t samples)
{
    return (uint32_t)((uint64_t)samples * 1000u / AUDIO_SAMPLE_RATE);
}

int bgm_play_src(const bgm_source_t *s, uint32_t key, uint32_t start_seq)
{
    bgm_stop();

    src = *s;
    have_src = true;
    memset(&cur, 0, sizeof(cur));
    bgm_loop_init(&cur.loop, src.loop ? src.loop_start : 0,
                  src.loop ? src.loop_end : 0, src.spr, src.nblocks);
    cur.seq = start_seq;
    cur.end_off = bgm_loop_end_off(&cur.loop, start_seq, src.spr);
    memset(&st, 0, sizeof(st));

    if (!audio_init()) return CD_ENOAUDIO;       // cannot fail after boot
    track_cur = key;
    if (!audio_start(bgm_fill)) { track_cur = 0; return CD_ENOAUDIO; }

    if (src.fmt == AFMT_CDDA) {
        uint32_t left = src.nblocks - start_seq;
        printf("  bgm: track %lu (%s), lba %lu to %lu (%lu:%02lu)\n",
               (unsigned long)key, src.name, (unsigned long)start_seq,
               (unsigned long)src.nblocks, (unsigned long)(left / 75u / 60u),
               (unsigned long)(left / 75u % 60u));
        return CD_OK;
    }
    printf("  bgm: track %lu (%s), %s, %lu blocks (%lu.%01lu s)%s%s\n",
           (unsigned long)key, src.name,
           src.fmt == AFMT_PCM16 ? "pcm16" : "adpcm4",
           (unsigned long)src.nblocks,
           (unsigned long)((uint64_t)src.nblocks * src.spr / AUDIO_SAMPLE_RATE),
           (unsigned long)((uint64_t)src.nblocks * src.spr * 10
                           / AUDIO_SAMPLE_RATE % 10),
           src.loop ? ", loop" : "", start_seq ? ", resumed" : "");
    if (src.loop && src.loop_end)
        printf("  bgm: loop [%lu.%03lu, %lu.%03lu) s, blocks %lu..%lu\n",
               (unsigned long)(ms_of(src.loop_start) / 1000u),
               (unsigned long)(ms_of(src.loop_start) % 1000u),
               (unsigned long)(ms_of(src.loop_end) / 1000u),
               (unsigned long)(ms_of(src.loop_end) % 1000u),
               (unsigned long)cur.loop.ls_blk, (unsigned long)cur.loop.le_blk);
    return CD_OK;
}

// -- disc source ------------------------------------------------------------

static bool __not_in_flash_func(disc_block)(uint32_t seq, cd_span_t *r)
{
    if (!disc_slot_get(seq, &r->a, &r->na)) return false;
    r->b = NULL; r->nb = 0;
    return true;
}

int bgm_play_disc(uint32_t key, uint32_t start_seq)
{
    const track_t *t = track_info();
    if (!t) return CD_ETRKFMT;
    bgm_source_t s = {
        .block = disc_block, .fmt = t->fmt, .spr = t->spr, .blk = t->blk,
        .nblocks = t->nblocks, .loop = t->loop, .name = "disc track",
        .loop_start = t->loop_start, .loop_end = t->loop_end,
    };
    return bgm_play_src(&s, key, start_seq);
}

int bgm_play_cdda(uint32_t key, uint32_t start_lba)
{
    const cdda_toc_t *t = cdda_toc();
    uint8_t n = cdda_track_of(start_lba);
    if (!t || !cdda_is_audio(n)) return CD_ENOAUDIO;
    bgm_source_t s = {
        .block = disc_block, .fmt = AFMT_CDDA, .spr = CDDA_SPR,
        .blk = ATAPI_CDDA_BYTES, .nblocks = t->audio_end[n], .loop = false,
        .name = "audio cd",
    };
    return bgm_play_src(&s, key, start_lba);
}

void bgm_stop(void)
{
    if (!audio_fill_is(bgm_fill)) return;
    cur.fading = true;
    if (!cur.ended) sleep_ms(AUDIO_FADE_OUT_MS);
    audio_stop();
}

void bgm_forget(void)
{
    bgm_stop();
    track_cur = 0;
    have_src = false;
}

void bgm_release(void)
{
    if (audio_fill_is(bgm_fill) && !src.loop && !cur.done) {
        track_cur = 0;                  // a jingle finishes; a repeat restarts
        have_src = false;
        return;
    }
    bgm_forget();
}

void bgm_hold(bool on)
{
    if (audio_fill_is(bgm_fill)) cur.hold = on;
}

bool bgm_active(void) { return audio_fill_is(bgm_fill); }

uint32_t bgm_track(void) { return bgm_active() ? track_cur : 0; }

bool bgm_ended(void) { return bgm_active() && cur.done; }

void bgm_report(void)
{
    if (!st.blocks && !st.silent) return;
    printf("  bgm: %lu blocks, %lu silent, %lu lap(s), worst isr %lu us\n",
           (unsigned long)st.blocks, (unsigned long)st.silent,
           (unsigned long)(have_src ? bgm_loop_laps(&cur.loop, cur.seq) : 0),
           (unsigned long)st.worst_isr_us);
}
