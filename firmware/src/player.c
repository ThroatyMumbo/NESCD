// player.c - see player.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "atapi.h"
#include "bgm.h"
#include "cdcore.h"
#include "cdda.h"
#include "disc.h"
#include "disctask.h"
#include "edn8.h"
#include "mailbox.h"
#include "player.h"
#include "usb_link.h"

#define POLL_MS       250u
#define BOOT_POLL_MS  750u           // a fresh ROM is still uploading its tiles
#define MAGIC_EVERY   8u             // polls between magic re-checks, 2 s
#define MAGIC_MISSES  4u
#define FAIL_WARN     8u
#define START_MS      5000u          // first chunk after a cold seek
#define STOP_MS       35000u

static bool armed;
static player_state_t state;
static uint8_t track;                // the one to play, or playing
static uint32_t play_start;          // the consumer's cursor while bgm is idle
static absolute_time_t due;
static bool req_valid;
static uint8_t req_last, magic_miss, read_fails, ticks;
static uint8_t shown[5], shown_len[2];
static bool shown_valid;

static const char *const state_name[] = {
    "no disc", "tray open", "loading", "stopped", "playing", "paused", "not an audio cd",
};

static uint32_t consumer(void) { return bgm_active() ? bgm_cursor() : play_start; }

static uint8_t cur_track(void)
{
    if (bgm_active()) {
        uint8_t t = cdda_track_of(bgm_cursor());
        if (t) return t;
    }
    return track;
}

static void set_state(player_state_t s) { state = s; }

// -- transport ---------------------------------------------------------------------

static bool cdda_running(void)
{
    return disctask_busy() && disctask_job() == &cdda_job && !cdda_medium_gone();
}

static int start_at(uint8_t t)
{
    const cdda_toc_t *toc = cdda_toc();
    if (!toc || !cdda_is_audio(t)) return CD_ENOAUDIO;
    uint32_t at = toc->start[t];

    bool reuse = cdda_running() && cdda_slots_hold(cdda_next_seq(), at, disc_slot_count());
    bgm_stop();
    play_start = at;
    disc_consumer = consumer;

    if (!reuse) {
        if (disctask_busy()) {
            disctask_stop(STOP_MS);
            if (disctask_busy()) return CD_EDISCIO;   // core1 still owns the bus
        }
        int rc = disctask_run(&cdda_job, at);
        if (rc != CD_OK) return rc;
        absolute_time_t give = make_timeout_time_ms(START_MS);
        while (disc_stat.blocks == 0 && disctask_busy() && !time_reached(give)) {
            usb_link_pump();
            sleep_ms(2);
        }
        if (disc_stat.blocks == 0) {
            int prc = disctask_stop(STOP_MS);
            return prc != CD_OK ? prc : CD_EDISCIO;
        }
    }
    int rc = bgm_play_cdda(t, at);
    if (rc != CD_OK) return rc;
    track = t;
    set_state(PL_PLAYING);
    return CD_OK;
}

static int do_stop(void)
{
    bgm_stop();
    int rc = CD_OK;
    if (cdda_running() || (disctask_busy() && disctask_job() == &cdda_job))
        rc = disctask_stop(STOP_MS);
    if (cdda_is_open()) set_state(PL_STOPPED);
    return rc;
}

static int do_pause(void)
{
    if (state == PL_PLAYING) { bgm_hold(true); set_state(PL_PAUSED); return CD_OK; }
    if (state == PL_PAUSED)  { bgm_hold(false); set_state(PL_PLAYING); return CD_OK; }
    return CD_ENOAUDIO;
}

// The neighboring audio track, skipping data ones; 0 at the end.
static uint8_t step_track(uint8_t from, int dir)
{
    const cdda_toc_t *toc = cdda_toc();
    if (!toc) return 0;
    int n = (int)from + dir;
    while (n >= (int)toc->first && n <= (int)toc->last) {
        if (cdda_is_audio((uint8_t)n)) return (uint8_t)n;
        n += dir;
    }
    return 0;
}

static int do_step(int dir)
{
    uint8_t t = step_track(cur_track(), dir);
    if (!t) return CD_ENOITEM;
    if (state == PL_PLAYING || state == PL_PAUSED) return start_at(t);
    if (state != PL_STOPPED) return CD_ENOAUDIO;
    track = t;
    return CD_OK;
}

static int do_play(uint8_t t)
{
    if (!cdda_is_open()) return CD_ENOAUDIO;
    if (t == 0) {
        if (state == PL_PAUSED) return do_pause();
        t = cur_track();
    }
    return start_at(t);
}

static int do_eject(void)
{
    do_stop();
    if (disctask_busy()) return CD_EDISCIO;
    cdda_close();
    disc_speed_reset();
    int arc = atapi_start_stop(ATAPI_SS_EJECT);
    if (arc != ATAPI_OK) return CD_EDISCIO;
    set_state(PL_TRAY_OPEN);
    track = 0;
    return CD_OK;
}

static int do_load(void)
{
    if (disctask_busy()) return CD_EDISCIO;
    int arc = atapi_start_stop(ATAPI_SS_LOAD);
    if (arc != ATAPI_OK) return CD_EDISCIO;
    set_state(PL_LOADING);
    return CD_OK;
}

static int command(uint8_t cmd, uint8_t param)
{
    switch (cmd) {
    case PLC_PLAY:  return do_play(param);
    case PLC_PAUSE: return do_pause();
    case PLC_STOP:  return cdda_is_open() ? do_stop() : CD_ENOAUDIO;
    case PLC_NEXT:  return do_step(+1);
    case PLC_PREV:  return do_step(-1);
    case PLC_EJECT: return do_eject();
    case PLC_LOAD:  return do_load();
    default:        return CD_ENOITEM;
    }
}

static const char *cmd_name(uint8_t cmd)
{
    static const char *const n[] = { "?", "play", "pause", "stop", "next", "prev", "eject", "load" };
    return cmd <= PLC_LOAD ? n[cmd] : "?";
}

// -- media edges ---------------------------------------------------------------------

void player_disc_in(void)
{
    const cdda_toc_t *toc = cdda_toc();
    track = cdda_first_audio();
    shown_valid = false;
    set_state(PL_STOPPED);
    if (!toc) return;
    if (armed) {
        int rc = start_at(track);
        if (rc != CD_OK) printf("player: %s\n", cd_strerror(rc));
    }
}

void player_disc_other(void)
{
    track = 0;
    set_state(PL_NOT_AUDIO);
}

void player_disc_out(void)
{
    bgm_stop();
    track = 0;
    if (state != PL_TRAY_OPEN) set_state(PL_NO_DISC);
}

void player_media(int st, bool shut_empty)
{
    if (st == 0) {
        if (state == PL_NO_DISC || state == PL_TRAY_OPEN || state == PL_LOADING)
            set_state(shut_empty ? PL_NO_DISC : PL_TRAY_OPEN);
        return;
    }
    if (st == 1 && !cdda_is_open() && (state == PL_NO_DISC || state == PL_TRAY_OPEN))
        set_state(PL_LOADING);
}

// -- the mailbox --------------------------------------------------------------------

bool player_magic_ok(void)
{
    uint8_t m[4];
    if (!edn8_is_open()) return false;
    if (mailbox_rd_at(PLAYER_MAGIC_PPU, m, 4) != CD_OK) return false;
    return memcmp(m, PLAYER_MAGIC, 4) == 0;
}

static void push_status(void)
{
    if (!edn8_is_open()) return;
    uint8_t t = cur_track(), m = 0, s = 0, lm = 0, ls = 0;
    const cdda_toc_t *toc = cdda_toc();
    if (toc && t) {
        if (bgm_active()) cdda_msf(bgm_cursor() - toc->start[t], &m, &s);
        cdda_msf(cdda_track_len(t), &lm, &ls);
    }
    uint8_t st[5] = { (uint8_t)state, t, toc ? toc->last : 0u, m, s };
    uint8_t len[2] = { lm, ls };
    if (!shown_valid || memcmp(len, shown_len, 2) != 0) {
        if (mailbox_wr_at(PLAYER_LEN_PPU, len, 2) != EDN8_OK) return;
        memcpy(shown_len, len, 2);
    }
    if (!shown_valid || memcmp(st, shown, 5) != 0) {
        if (mailbox_wr_at(PLAYER_STAT_PPU, st, 5) != EDN8_OK) return;
        memcpy(shown, st, 5);
    }
    shown_valid = true;
}

void player_arm(bool fresh)
{
    armed = true;
    req_valid = false;
    shown_valid = false;
    magic_miss = read_fails = ticks = 0;
    due = make_timeout_time_ms(fresh ? BOOT_POLL_MS : POLL_MS);
    if (edn8_is_open()) mailbox_status_wr(0);
    if (cdda_is_open() && state == PL_STOPPED) {
        int rc = start_at(track);
        if (rc != CD_OK) printf("player: %s\n", cd_strerror(rc));
    }
}

void player_disarm(void)
{
    if (!armed) return;
    armed = false;
    do_stop();
    if (edn8_is_open()) mailbox_status_wr(0);
}

bool player_armed(void) { return armed; }

bool player_poll(void)
{
    bool printed = false;
    if (state == PL_PLAYING && bgm_ended()) {
        bgm_stop();
        track = cdda_first_audio();
        set_state(PL_STOPPED);
        printf("\nplayer: end of the disc\n");
        printed = true;
    }
    // Stopped from outside (bare B, M): the state follows the DAC.
    if ((state == PL_PLAYING || state == PL_PAUSED) && !bgm_active())
        set_state(PL_STOPPED);
    if (!armed || !time_reached(due)) return printed;
    due = make_timeout_time_ms(POLL_MS);
    if (!edn8_is_open()) return printed;

    // A console reset re-uploads CHR and blanks the page before the ROM's
    // first frame restores it, so one miss means nothing.
    if (++ticks >= MAGIC_EVERY) {
        ticks = 0;
        if (!player_magic_ok()) {
            if (++magic_miss >= MAGIC_MISSES) {
                printf("\nplayer: the CD player ROM is gone - disarmed\n");
                player_disarm();
                return true;
            }
        } else {
            magic_miss = 0;
        }
    }

    uint8_t mb[2];
    int rc = mailbox_rd(mb);
    if (rc != CD_OK) {
        if (++read_fails != FAIL_WARN) return printed;
        printf("\nplayer: mailbox unreadable - %s\n", cd_strerror(rc));
        return true;
    }
    if (read_fails >= FAIL_WARN) printf("\nplayer: mailbox readable again\n");
    read_fails = 0;

    // The first read after arming only latches: a request raised before the
    // host was here would otherwise fire now.
    if (!req_valid) { req_valid = true; req_last = mb[0]; }
    if (mb[0] != req_last) {
        req_last = mb[0];
        if (mb[0]) {
            uint8_t cmd = mb[0] & 0x0Fu;
            printf("\nplayer: %s", cmd_name(cmd));
            if (cmd == PLC_PLAY && mb[1]) printf(" %u", mb[1]);
            rc = command(cmd, mb[1]);
            if (rc != CD_OK) printf(" - %s", cd_strerror(rc));
            printf("\n");
            mailbox_status_wr(rc == CD_OK ? mb[0] : (uint8_t)(mb[0] | PLAYER_ANS_FAIL));
            printed = true;
        }
    }
    push_status();
    return printed;
}

bool player_reap(int rc)
{
    if (rc == CD_OK || rc == CD_EMEDIUM) return false;   // played out, or the tray poll's
    printf("\nplayer: %s\n", cd_strerror(rc));
    bgm_stop();
    if (cdda_is_open()) set_state(PL_STOPPED);
    return true;
}

// -- console ----------------------------------------------------------------------

static void show_toc(void)
{
    const cdda_toc_t *toc = cdda_toc();
    if (!toc) { printf("  no audio cd open\n"); return; }
    uint8_t m, s;
    for (uint32_t n = toc->first; n <= toc->last; n++) {
        cdda_msf(cdda_is_audio((uint8_t)n) ? cdda_track_len((uint8_t)n)
                 : (n < toc->last ? toc->start[n + 1u] : toc->leadout) - toc->start[n], &m, &s);
        printf("  %2lu  lba %6lu  %2u:%02u  %s\n", (unsigned long)n,
               (unsigned long)toc->start[n], m, s,
               (toc->ctrl[n] & 0x04u) ? "data" : "audio");
    }
    printf("  lead-out at lba %lu\n", (unsigned long)toc->leadout);
}

void player_report(void)
{
    printf("  player:   %s, %s", armed ? "armed" : "off", state_name[state]);
    const cdda_toc_t *toc = cdda_toc();
    if (toc) {
        uint8_t t = cur_track(), m = 0, s = 0;
        if (bgm_active() && t) cdda_msf(bgm_cursor() - toc->start[t], &m, &s);
        printf(", track %u/%u at %u:%02u", t, toc->last, m, s);
        if (toc->cap != CDDA_CAP_UNKNOWN)
            printf(", drive: cd-da %s, stream %s",
                   (toc->cap & CDDA_CAP_CMDS) ? "yes" : "no",
                   (toc->cap & CDDA_CAP_ACCURATE) ? "accurate" : "NOT accurate");
        printf(", read cd flags %02X", cdda_read_flags());
    }
    printf("\n");
}

void player_console(const char *arg)
{
    int rc = CD_OK;
    switch (*arg) {
    case 0:   player_report(); return;
    case 't': show_toc(); return;
    case 'p': rc = do_play((uint8_t)strtoul(arg + 1, NULL, 10)); break;
    case 'h': rc = do_pause(); break;
    case 's': rc = cdda_is_open() ? do_stop() : CD_ENOAUDIO; break;
    case 'n': rc = do_step(+1); break;
    case 'v': rc = do_step(-1); break;
    case 'e': rc = do_eject(); break;
    case 'l': rc = do_load(); break;
    default:
        printf("  usage: C [t | p [track] | h | s | n | v | e | l]\n");
        return;
    }
    if (rc != CD_OK) printf("  player: %s\n", cd_strerror(rc));
    player_report();
}
