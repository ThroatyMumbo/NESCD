// cdda.c - see cdda.h.

#include <assert.h>
#include <string.h>

#include "atapi.h"
#include "audiofmt.h"
#include "cdda.h"
#include "disc.h"

static cdda_toc_t toc;
static bool opened;
static volatile uint32_t stop_req, eos_flag, gone_flag, next_seq;
static uint8_t rcd_flags = ATAPI_RCD_USER;
static uint8_t probe_buf[ATAPI_CDDA_BYTES] __attribute__((aligned(4)));

static_assert(CDDA_CHUNK * ATAPI_CDDA_BYTES <= 0xFFFEu, "a chunk is one burst");
static_assert(CDDA_CHUNK * ATAPI_CDDA_BYTES <= DISC_CHUNK_SECTORS * DISC_SECTOR,
              "a chunk fits the read buffer");
static_assert(ATAPI_CDDA_BYTES <= DISC_BLOCK_MAX, "a sector is a slot");
static_assert(ATAPI_CDDA_BYTES == 4u * CDDA_SPR, "588 stereo frames a sector");

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Some 1990s firmware refuses 0x10 for audio and wants every field asked for;
// the bytes come back the same either way.
static int read_cd(uint32_t lba, uint32_t n, void *buf, size_t *got)
{
    size_t want = (size_t)n * ATAPI_CDDA_BYTES;
    int rc = atapi_read_cd(lba, n, rcd_flags, buf, want, got);
    if (rc == ATAPI_ECHECK && rcd_flags == ATAPI_RCD_USER
        && atapi_sense_key == 0x05 && atapi_sense_asc == 0x24) {
        rcd_flags = ATAPI_RCD_ALL;
        rc = atapi_read_cd(lba, n, rcd_flags, buf, want, got);
    }
    return rc;
}

static bool probe(uint32_t lba)
{
    size_t got = 0;
    return read_cd(lba, 1, probe_buf, &got) == ATAPI_OK && got == ATAPI_CDDA_BYTES;
}

int cdda_parse_toc(const uint8_t *p, size_t got, cdda_toc_t *t)
{
    if (got < ATAPI_TOC_HDR + ATAPI_TOC_DESC) return CD_ESHAPE;
    size_t len = (((size_t)p[0] << 8) | p[1]) + 2u;
    if (len > got) len = got;

    memset(t, 0, sizeof(*t));
    t->cap = CDDA_CAP_UNKNOWN;
    t->first = p[2];
    t->last = p[3];
    if (t->first < 1u || t->last > 99u || t->first > t->last) return CD_ESHAPE;

    bool leadout = false;
    uint8_t seen[CDDA_TRACKS] = { 0 };
    for (size_t off = ATAPI_TOC_HDR; off + ATAPI_TOC_DESC <= len; off += ATAPI_TOC_DESC) {
        const uint8_t *d = p + off;
        uint8_t n = d[2];
        if (n == ATAPI_TOC_LEADOUT) { t->leadout = be32(d + 4); leadout = true; continue; }
        if (n < t->first || n > t->last) continue;
        t->start[n] = be32(d + 4);
        t->ctrl[n] = d[1] & 0x0Fu;
        seen[n] = 1;
    }
    if (!leadout) return CD_ESHAPE;
    for (uint32_t n = t->first; n <= t->last; n++) {
        if (!seen[n]) return CD_ESHAPE;
        if (n > t->first && t->start[n] < t->start[n - 1u]) return CD_ESHAPE;
        if (t->start[n] >= t->leadout) return CD_ESHAPE;
    }

    // Backwards: an audio run stops at the next data track or the lead-out.
    uint32_t end = t->leadout;
    for (uint32_t n = t->last; n >= t->first; n--) {
        if (t->ctrl[n] & 0x04u) {
            end = t->start[n];
            if (n > t->first && !(t->ctrl[n - 1u] & 0x04u)
                && end > t->start[n - 1u] + CDDA_SESSION_GAP)
                end -= CDDA_SESSION_GAP;
        } else {
            t->audio_end[n] = end;
        }
        if (n == t->first) break;
    }
    return CD_OK;
}

int cdda_open(void)
{
    opened = false;
    if (atapi_wait_ready(30000) != ATAPI_OK) return CD_EDISCIO;

    uint8_t *buf = disc_chunk_buf();
    size_t got = 0;
    int arc = atapi_read_toc(buf, ATAPI_TOC_MAX, &got);
    if (arc != ATAPI_OK) return CD_EDISCIO;
    int rc = cdda_parse_toc(buf, got, &toc);
    if (rc != CD_OK) return rc;
    if (toc.ctrl[toc.first] & 0x04u) return CD_ENOAUDIO;

    got = 0;
    if (atapi_mode_sense_cap(buf, 64, &got) == ATAPI_OK && got >= 14u
        && (buf[8] & 0x3Fu) == 0x2Au)
        toc.cap = buf[8 + 5];

    if ((rc = disc_prepare_at(toc.start[toc.first], probe)) != CD_OK) return rc;
    if ((rc = disc_slots_open(ATAPI_CDDA_BYTES, CDDA_SLOTS_MIN)) != CD_OK) return rc;
    opened = true;
    return CD_OK;
}

void cdda_close(void) { opened = false; }
bool cdda_is_open(void) { return opened; }
const cdda_toc_t *cdda_toc(void) { return opened ? &toc : NULL; }

bool cdda_is_audio(uint8_t track)
{
    return opened && track >= toc.first && track <= toc.last && !(toc.ctrl[track] & 0x04u);
}

uint8_t cdda_first_audio(void)
{
    if (!opened) return 0;
    for (uint32_t n = toc.first; n <= toc.last; n++)
        if (!(toc.ctrl[n] & 0x04u)) return (uint8_t)n;
    return 0;
}

uint8_t cdda_track_of(uint32_t lba)
{
    if (!opened || lba >= toc.leadout) return 0;
    for (uint32_t n = toc.last; n >= toc.first; n--) {
        if (lba >= toc.start[n]) return (uint8_t)n;
        if (n == toc.first) break;
    }
    return 0;
}

uint32_t cdda_track_len(uint8_t track)
{
    if (!cdda_is_audio(track)) return 0;
    uint32_t next = track < toc.last ? toc.start[track + 1u] : toc.leadout;
    if (next > toc.audio_end[track]) next = toc.audio_end[track];
    return next - toc.start[track];
}

void cdda_msf(uint32_t sectors, uint8_t *min, uint8_t *sec)
{
    uint32_t s = sectors / 75u;
    *min = (uint8_t)(s / 60u > 99u ? 99u : s / 60u);
    *sec = (uint8_t)(s % 60u);
}

bool cdda_slots_hold(uint32_t next_seq, uint32_t at, uint32_t slots)
{
    int32_t d = (int32_t)(next_seq - at);
    return d > 2 * (int32_t)CDDA_CHUNK && d < (int32_t)slots - 2 * (int32_t)CDDA_CHUNK;
}

// -- producer ------------------------------------------------------------------

void cdda_stop(void) { stop_req = 1u; }
uint32_t cdda_next_seq(void) { return __atomic_load_n(&next_seq, __ATOMIC_ACQUIRE); }
bool cdda_eos(void) { return __atomic_load_n(&eos_flag, __ATOMIC_ACQUIRE) != 0u; }
bool cdda_medium_gone(void) { return __atomic_load_n(&gone_flag, __ATOMIC_ACQUIRE) != 0u; }
uint8_t cdda_read_flags(void) { return rcd_flags; }

void cdda_reset(void)
{
    stop_req = eos_flag = gone_flag = 0;
    disc_stat_reset();
}

const disc_job_t cdda_job = { cdda_fill, cdda_stop, cdda_reset, "audio cd" };

int cdda_fill(uint32_t start)
{
    if (!opened) return CD_ENOAUDIO;
    if (!disc_slot_count()) return CD_EPSRAM;
    uint8_t track = cdda_track_of(start);
    if (!cdda_is_audio(track)) return CD_ESHAPE;

    uint8_t *chunk = disc_chunk_buf();
    uint32_t end = toc.audio_end[track];
    uint32_t seq = start, tries = 0, spins = 0;
    bool slowed = false;
    int rc = CD_OK;

    disc_stat.running = 1u;
    __atomic_store_n(&next_seq, seq, __ATOMIC_RELEASE);

    while (!stop_req) {
        if (!disc_wait_depth(seq, start, CDDA_DEPTH, &stop_req, &gone_flag)) {
            if (cdda_medium_gone()) rc = CD_EMEDIUM;
            break;
        }
        if (seq >= end) { __atomic_store_n(&eos_flag, 1u, __ATOMIC_RELEASE); break; }
        uint32_t n = end - seq;
        if (n > CDDA_CHUNK) n = CDDA_CHUNK;

        size_t got = 0;
        uint32_t t0 = disc_now_ms ? disc_now_ms() : 0;
        int arc = read_cd(seq, n, chunk, &got);
        disc_stat_read_ms(t0);

        if (arc != ATAPI_OK || got != (size_t)n * ATAPI_CDDA_BYTES) {
            disc_stat_read_failed(arc);
            bool chk = arc == ATAPI_ECHECK;
            if (chk && atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A) {
                __atomic_store_n(&gone_flag, 1u, __ATOMIC_RELEASE);
                rc = CD_EMEDIUM;
                break;
            }
            // Spun down under a long pause: the spin-up is not a retry.
            if (chk && atapi_sense_key == 0x02 && atapi_sense_asc == 0x04 && spins++ < 2u) {
                atapi_wait_ready(30000);
                continue;
            }
            // 5/64 past the settle window is a drive that will not extract at
            // this speed: try once at 1x.
            if (chk && atapi_sense_key == 0x05 && atapi_sense_asc == 0x64 && !slowed) {
                slowed = true;
                disc_stat.retries++;
                atapi_set_cd_speed(ATAPI_SPEED_1X);
                atapi_wait_ready(5000);
                continue;
            }
            if (end - seq <= CDDA_TAIL) { __atomic_store_n(&eos_flag, 1u, __ATOMIC_RELEASE); break; }
            if (++tries > CDDA_RETRIES) { rc = CD_EDISCIO; break; }
            disc_stat.retries++;
            atapi_wait_ready(2000);
            continue;
        }
        tries = 0;
        disc_stat.chunks++;

        for (uint32_t i = 0; i < n; i++) {
            disc_slot_put(seq, chunk + (size_t)i * ATAPI_CDDA_BYTES);
            seq++;
            disc_stat.blocks++;
            __atomic_store_n(&next_seq, seq, __ATOMIC_RELEASE);
        }
    }
    disc_stat.running = 0u;
    return rc;
}
