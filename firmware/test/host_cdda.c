// host_cdda.c - run the audio-cd reader against a synthetic disc on the PC:
// the TOC parse, the producer through the slot ring across chunk and track
// seams, the run's end at a data session, and every failure the drive can
// throw at it.
//
//   gcc -O2 -I src -I test/stub -o /tmp/host_cdda test/host_cdda.c
//       src/cdda.c src/disc.c src/cdcore.c
//   /tmp/host_cdda

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atapi.h"
#include "cdcore.h"
#include "cdda.h"
#include "disc.h"

// -- the disc ---------------------------------------------------------------------
//
// Three audio tracks, then a data track a session gap away (CD-Extra), so the
// audio run ends 11400 sectors before the data track starts.

#define T1 0u
#define T2 3000u
#define T3 6000u
#define T4 (9000u + CDDA_SESSION_GAP)
#define LEADOUT 25000u
#define AUDIO_END 9000u

static uint32_t reads, reads10, fail_every, gone_at, speed_kb, waits;
static bool tail_fails, want_f8, spin_once, slow_until_1x, data_first, toc_short;

uint8_t atapi_sense_key, atapi_sense_asc, atapi_sense_ascq;

const char *edn8_strerror(int rc) { (void)rc; return "stub error"; }   // cdcore.c's fallthrough

static void sense(uint8_t k, uint8_t a, uint8_t q)
{
    atapi_sense_key = k; atapi_sense_asc = a; atapi_sense_ascq = q;
}

int atapi_wait_ready(uint32_t timeout_ms) { (void)timeout_ms; waits++; return ATAPI_OK; }
int atapi_set_cd_speed(uint16_t kb) { speed_kb = kb; return ATAPI_OK; }
int atapi_test_unit_ready(void) { return ATAPI_OK; }

int atapi_read10(uint32_t lba, uint16_t blocks, void *buf, size_t maxlen, size_t *got)
{
    (void)lba; (void)blocks; (void)buf; (void)maxlen;
    reads10++;
    sense(5, 0x64, 0);                       // illegal mode for this track
    *got = 0;
    return ATAPI_ECHECK;
}

static void put_desc(uint8_t *d, uint8_t ctrl, uint8_t track, uint32_t lba)
{
    d[0] = 0; d[1] = (uint8_t)(0x10 | ctrl); d[2] = track; d[3] = 0;
    d[4] = (uint8_t)(lba >> 24); d[5] = (uint8_t)(lba >> 16);
    d[6] = (uint8_t)(lba >> 8);  d[7] = (uint8_t)lba;
}

int atapi_read_toc(void *buf, size_t len, size_t *got)
{
    uint8_t t[ATAPI_TOC_HDR + 5 * ATAPI_TOC_DESC];
    size_t n = sizeof(t);
    t[0] = (uint8_t)((n - 2) >> 8); t[1] = (uint8_t)(n - 2);
    t[2] = 1; t[3] = 4;
    put_desc(t + 4,  data_first ? 4 : 0, 1, T1);
    put_desc(t + 12, 0, 2, T2);
    put_desc(t + 20, 0, 3, T3);
    put_desc(t + 28, 4, 4, T4);
    put_desc(t + 36, 0, ATAPI_TOC_LEADOUT, LEADOUT);
    if (toc_short) n = 20;                   // a drive that answers short
    if (n > len) n = len;
    memcpy(buf, t, n);
    *got = n;
    return ATAPI_OK;
}

int atapi_mode_sense_cap(void *buf, size_t len, size_t *got)
{
    uint8_t p[8 + 16] = { 0 };
    p[8] = 0x2A; p[9] = 14; p[8 + 5] = 0x03;
    size_t n = sizeof(p) < len ? sizeof(p) : len;
    memcpy(buf, p, n);
    *got = n;
    return ATAPI_OK;
}

// Every byte of a sector is a function of its LBA, the first four are the LBA.
static void fill_sector(uint8_t *p, uint32_t lba)
{
    p[0] = (uint8_t)lba; p[1] = (uint8_t)(lba >> 8); p[2] = (uint8_t)(lba >> 16); p[3] = 0;
    for (uint32_t i = 4; i < ATAPI_CDDA_BYTES; i++) p[i] = (uint8_t)(lba * 7u + i);
}

int atapi_read_cd(uint32_t lba, uint32_t nsec, uint8_t flags, void *buf,
                  size_t maxlen, size_t *got)
{
    *got = 0;
    reads++;
    if (want_f8 && flags != ATAPI_RCD_ALL) { sense(5, 0x24, 0); return ATAPI_ECHECK; }
    if (gone_at && reads > gone_at) { sense(2, 0x3A, 0); return ATAPI_ECHECK; }
    if (spin_once) { spin_once = false; sense(2, 0x04, 1); return ATAPI_ECHECK; }
    if (slow_until_1x && speed_kb != ATAPI_SPEED_1X) { sense(5, 0x64, 0); return ATAPI_ECHECK; }
    if (fail_every && reads % fail_every == 0) { sense(3, 0x02, 1); return ATAPI_ECHECK; }
    if (lba + nsec > AUDIO_END + 2u) { sense(5, 0x64, 0); return ATAPI_ECHECK; }
    if (tail_fails && lba + nsec > AUDIO_END - 2u) { sense(3, 0x11, 0); return ATAPI_ECHECK; }
    size_t want = (size_t)nsec * ATAPI_CDDA_BYTES;
    if (want > maxlen) want = maxlen;
    for (uint32_t i = 0; i < nsec; i++) fill_sector((uint8_t *)buf + (size_t)i * ATAPI_CDDA_BYTES, lba + i);
    *got = want;
    return ATAPI_OK;
}

static uint32_t now_ms(void) { return 0; }
static uint32_t pauses;
static void pause_ms(uint32_t ms) { (void)ms; pauses++; }

// -- checks --------------------------------------------------------------------------

static int fails;
static void check(int cond, const char *what)
{
    printf("  %-64s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

// -- the consumer --------------------------------------------------------------------

static uint32_t cons_seq, wrong, checked, stop_at;

static uint32_t consumer(void) { return cons_seq; }

static void consume_one(void)
{
    if (cdda_next_seq() <= cons_seq) return;
    const uint8_t *p; uint32_t n;
    uint8_t ref[ATAPI_CDDA_BYTES];
    fill_sector(ref, cons_seq);
    if (!disc_slot_get(cons_seq, &p, &n) || n != ATAPI_CDDA_BYTES || memcmp(p, ref, n) != 0) wrong++;
    else checked++;
    if (cdda_next_seq() > cons_seq + CDDA_DEPTH + CDDA_CHUNK) { printf("  producer ran ahead\n"); fails++; }
    cons_seq++;
    if (stop_at && cons_seq >= stop_at) cdda_stop();
}

static int run(uint32_t start, uint32_t stop)
{
    cons_seq = start; stop_at = stop;
    wrong = checked = 0;
    cdda_reset();
    int rc = cdda_fill(start);
    while (cdda_next_seq() > cons_seq && !cdda_medium_gone()) consume_one();
    return rc;
}

int main(void)
{
    disc_now_ms = now_ms;
    disc_pause_ms = pause_ms;
    static uint8_t aring[DISC_SLOTS_MAX * DISC_BLOCK_MAX];
    disc_slots_attach(aring, sizeof aring);

    // -- the toc ---------------------------------------------------------------
    uint8_t raw[ATAPI_TOC_MAX];
    size_t got = 0;
    cdda_toc_t t;
    atapi_read_toc(raw, sizeof raw, &got);
    check(cdda_parse_toc(raw, got, &t) == CD_OK, "toc: parses");
    check(t.first == 1 && t.last == 4 && t.leadout == LEADOUT, "toc: first, last, lead-out");
    check(t.start[2] == T2 && t.start[3] == T3 && t.start[4] == T4, "toc: track starts by number");
    check(!(t.ctrl[1] & 4) && (t.ctrl[4] & 4), "toc: control bits");
    check(t.audio_end[1] == AUDIO_END && t.audio_end[3] == AUDIO_END && t.audio_end[4] == 0,
          "toc: the audio run ends a session gap before the data track");
    check(cdda_parse_toc(raw, 11, &t) == CD_ESHAPE, "toc: too short refused");
    uint8_t bad[ATAPI_TOC_MAX];
    memcpy(bad, raw, got); bad[36 + 2] = 5;          // lead-out renamed
    check(cdda_parse_toc(bad, got, &t) == CD_ESHAPE, "toc: no lead-out refused");
    memcpy(bad, raw, got); bad[20 + 6] = 0; bad[20 + 7] = 100;   // track 3 at lba 100, before track 2
    check(cdda_parse_toc(bad, got, &t) == CD_ESHAPE, "toc: descending starts refused");
    toc_short = true;
    atapi_read_toc(raw, sizeof raw, &got);
    check(cdda_parse_toc(raw, got, &t) == CD_ESHAPE, "toc: a short answer missing tracks refused");
    toc_short = false;

    // -- open --------------------------------------------------------------------
    data_first = true;
    check(cdda_open() == CD_ENOAUDIO, "open: a data-first disc is not an audio cd");
    data_first = false;
    reads = reads10 = 0;
    int rc = cdda_open();
    check(rc == CD_OK && cdda_is_open(), "open: the disc");
    check(reads10 == 0 && reads >= 1, "open: settled the speed window with READ CD, never READ(10)");
    check(speed_kb == DISC_READ_SPEED_X * ATAPI_SPEED_1X, "open: read speed set once");
    check(cdda_toc()->cap == 0x03, "open: capability page read");
    check(disc_slot_count() >= CDDA_SLOTS_MIN, "open: slots sized for 2352 B sectors");
    check(cdda_first_audio() == 1, "first audio track is 1");
    check(cdda_track_of(0) == 1 && cdda_track_of(T2 - 1) == 1 && cdda_track_of(T2) == 2
          && cdda_track_of(AUDIO_END - 1) == 3 && cdda_track_of(T4) == 4
          && cdda_track_of(LEADOUT) == 0, "track_of at every boundary");
    check(cdda_track_len(1) == T2 - T1 && cdda_track_len(3) == AUDIO_END - T3 && cdda_track_len(4) == 0,
          "track_len stops at the audio end");
    uint8_t m, s;
    cdda_msf(3000, &m, &s);
    check(m == 0 && s == 40, "msf: 3000 sectors is 0:40");

    disc_idle = consume_one;
    disc_consumer = consumer;

    // -- the producer ----------------------------------------------------------------
    rc = run(0, T2 + 200u);
    check(rc == CD_OK && wrong == 0 && checked >= T2 + 200u, "fill: across chunk and track seams, every sector == its lba");
    check(disc_stat.chunks > 0 && disc_stat.retries == 0, "fill: no retries");

    rc = run(T3, 0);
    check(rc == CD_OK && cdda_eos() && cons_seq == AUDIO_END && wrong == 0,
          "fill: the run ends at the audio end, not the data track or lead-out");

    tail_fails = true;
    rc = run(T3, 0);
    check(rc == CD_OK && cdda_eos() && cdda_next_seq() >= AUDIO_END - CDDA_TAIL && wrong == 0,
          "fill: unreadable sectors before the end are the end");
    tail_fails = false;

    reads = 0; gone_at = 3;
    rc = run(0, 0);
    check(rc == CD_EMEDIUM && cdda_medium_gone(), "fill: tray open -> CD_EMEDIUM");
    check(reads == gone_at + 1u, "fill: exactly one read after the medium went");
    gone_at = 0;

    reads = 0; fail_every = 2;
    rc = run(0, 400);
    check(rc == CD_OK && disc_stat.retries > 0 && wrong == 0, "fill: read errors retried, sectors still match");
    fail_every = 0;

    spin_once = true; waits = 0;
    rc = run(T2, T2 + 100u);
    check(rc == CD_OK && disc_stat.retries == 0 && waits >= 1 && wrong == 0,
          "fill: a spun-down drive is waited for, not counted as a retry");

    slow_until_1x = true;
    rc = run(T2, T2 + 100u);
    check(rc == CD_OK && speed_kb == ATAPI_SPEED_1X && disc_stat.retries == 1 && wrong == 0,
          "fill: 5/64 drops to 1x once");
    slow_until_1x = false;

    want_f8 = true;
    rc = run(0, 100);
    check(rc == CD_OK && cdda_read_flags() == ATAPI_RCD_ALL && wrong == 0,
          "fill: READ CD falls back to 0xF8 on 5/24");
    want_f8 = false;

    // -- the reuse window ----------------------------------------------------------
    uint32_t slots = disc_slot_count();
    check(cdda_slots_hold(5000, 4900, slots) && cdda_slots_hold(5000, 5000 - slots + 2 * CDDA_CHUNK + 1, slots),
          "reuse: a sector the ring holds needs no re-read");
    check(!cdda_slots_hold(5000, 5000 - CDDA_CHUNK, slots) && !cdda_slots_hold(5000, 5100, slots)
          && !cdda_slots_hold(5000, 5000 - slots, slots), "reuse: the chunk in flight, ahead, and lapped are out");

    disc_idle = NULL;
    disc_consumer = NULL;
    printf(fails ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
