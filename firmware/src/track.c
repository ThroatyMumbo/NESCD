// track.c - the disc track producer.

#include <string.h>

#include "atapi.h"
#include "audiofmt.h"
#include "bgmloop.h"
#include "disc.h"
#include "track.h"

#define TRACK_RETRIES 2u

static track_t t;
static bool opened;
static volatile uint32_t stop_req, eos_flag, gone_flag, next_seq;
static uint8_t carry[DISC_BLOCK_MAX] __attribute__((aligned(4)));

uint32_t (*track_consumer)(void);
track_stat_t track_stat;

static int check_hdr(const trk_hdr_t *h, uint32_t sectors)
{
    if (memcmp(h->magic, TRACK_MAGIC, 8) != 0) return CD_ETRKFMT;
    if (h->version != TRACK_VER) return CD_EVERSION;
    if (h->nframes < 1u || h->nrecords != 0 || h->payload_bytes != 0) return CD_ETRKFMT;
    if (h->audio_format != AFMT_PCM16 && h->audio_format != AFMT_ADPCM4) return CD_ETRKFMT;
    if (!h->audio_spr || !h->audio_block) return CD_ETRKFMT;
    if (h->audio_block > DISC_BLOCK_MAX) return CD_ETRKFMT;
    if (h->table_off != sizeof(*h) || h->payload_off != sizeof(*h) + 4u) return CD_ETRKFMT;
    if (h->audio_off != TRACK_DATA) return CD_ETRKFMT;
    if ((uint64_t)h->nframes * h->audio_block != h->audio_bytes) return CD_ETRKFMT;
    if ((uint64_t)TRACK_DATA + h->audio_bytes > (uint64_t)sectors * DISC_SECTOR) return CD_ETRKFMT;
    if (h->loop_end) {
        if (!(h->flags & TRACK_F_LOOP)) return CD_ETRKFMT;
        if (h->loop_start % h->audio_spr || h->loop_start >= h->loop_end) return CD_ETRKFMT;
        if ((uint64_t)h->loop_end > (uint64_t)h->nframes * h->audio_spr) return CD_ETRKFMT;
        // The producer reads to the array end and seeks back, so the range's
        // last block must be the last stored one; mktrack.py trims there.
        if ((h->loop_end - 1u) / h->audio_spr != h->nframes - 1u) return CD_ETRKFMT;
    }
    return CD_OK;
}

int track_open(uint32_t lba, uint32_t sectors)
{
    opened = false;
    int rc = disc_prepare_at(lba);
    if (rc != CD_OK) return rc;

    uint8_t *buf = disc_chunk_buf();
    if ((rc = disc_read_sectors(lba, 1, buf)) != CD_OK) return rc;
    trk_hdr_t h;
    memcpy(&h, buf, sizeof(h));
    if ((rc = check_hdr(&h, sectors)) != CD_OK) return rc;

    t.lba = lba; t.sectors = sectors;
    t.nblocks = h.nframes; t.blk = h.audio_block;
    t.fmt = h.audio_format; t.spr = h.audio_spr;
    t.loop = (h.flags & TRACK_F_LOOP) != 0;
    t.loop_start = t.loop ? h.loop_start : 0;
    t.loop_end = t.loop ? h.loop_end : 0;

    if ((rc = disc_slots_open(t.blk, TRACK_SLOTS_MIN)) != CD_OK) return rc;
    opened = true;
    return CD_OK;
}

bool track_is_open(void) { return opened; }
const track_t *track_info(void) { return opened ? &t : NULL; }
void track_stop(void) { stop_req = 1u; }
uint32_t track_next_seq(void) { return __atomic_load_n(&next_seq, __ATOMIC_ACQUIRE); }
bool track_eos(void) { return __atomic_load_n(&eos_flag, __ATOMIC_ACQUIRE) != 0u; }
bool track_medium_gone(void) { return __atomic_load_n(&gone_flag, __ATOMIC_ACQUIRE) != 0u; }

void track_reset(void)
{
    stop_req = eos_flag = gone_flag = 0;
    memset(&track_stat, 0, sizeof(track_stat));
}

// Hold TRACK_DEPTH blocks ahead of the consumer: a steady trickle rather than
// a fill and a long idle, which makes the drive park and re-seek. The producer
// owns the drive while it waits, so the tray poll has to happen here.
static bool wait_depth(uint32_t seq, uint32_t start_seq)
{
    uint32_t parked_at = disc_now_ms ? disc_now_ms() : 0;
    for (;;) {
        uint32_t c = track_consumer ? track_consumer() : start_seq;
        // Signed: a consumer that ran ahead through holes must pull, not park.
        if ((int32_t)(seq - c) < (int32_t)TRACK_DEPTH) return true;
        if (stop_req) return false;
        if (!disc_heartbeat(&parked_at)) {
            __atomic_store_n(&gone_flag, 1u, __ATOMIC_RELEASE);
            return false;
        }
        if (disc_idle) disc_idle();
    }
}

// A range's last block is the array's last (check_hdr), so the producer always
// reads to the end and seeks back to ls_blk -- 0 when the whole array loops.
static void seek_blk(uint32_t blk, uint32_t *lba, uint32_t *skip)
{
    uint32_t byte = TRACK_DATA + blk * t.blk;
    *lba = t.lba + byte / DISC_SECTOR;
    *skip = byte % DISC_SECTOR;
}

int track_fill(uint32_t start_seq)
{
    if (!opened) return CD_ETRKFMT;
    if (!disc_slot_count()) return CD_EPSRAM;

    uint8_t *chunk = disc_chunk_buf();
    bgm_loop_t loop;
    bgm_loop_init(&loop, t.loop_start, t.loop_end, t.spr, t.nblocks);
    // The walk below is this fold's sequential form: both step to ls_blk.
    uint32_t seq = start_seq, idx = bgm_loop_block(&loop, seq);
    uint32_t lba, skip;
    seek_blk(idx, &lba, &skip);
    uint32_t end = t.lba + t.sectors;
    uint32_t carry_n = 0, tries = 0;
    int rc = CD_OK;

    track_stat.running = 1u;
    __atomic_store_n(&next_seq, seq, __ATOMIC_RELEASE);

    while (!stop_req) {
        if (!wait_depth(seq, start_seq)) {
            if (track_medium_gone()) rc = CD_EMEDIUM;
            break;
        }

        if (lba >= end) {                        // the array ends on a sector
            if (!t.loop) { __atomic_store_n(&eos_flag, 1u, __ATOMIC_RELEASE); break; }
            seek_blk(loop.ls_blk, &lba, &skip);
            carry_n = 0; idx = loop.ls_blk;
            track_stat.laps++;
            continue;
        }
        uint32_t n = end - lba;
        if (n > DISC_CHUNK_SECTORS) n = DISC_CHUNK_SECTORS;

        size_t got = 0;
        uint32_t t0 = disc_now_ms ? disc_now_ms() : 0;
        int arc = atapi_read10(lba, (uint16_t)n, chunk, (size_t)n * DISC_SECTOR, &got);
        uint32_t ms = (disc_now_ms ? disc_now_ms() : 0) - t0;
        if (ms > track_stat.worst_read_ms) track_stat.worst_read_ms = ms;

        if (arc != ATAPI_OK || got != (size_t)n * DISC_SECTOR) {
            track_stat.last_rc = arc;
            track_stat.last_sense = ((uint32_t)atapi_sense_key << 16)
                                  | ((uint32_t)atapi_sense_asc << 8) | atapi_sense_ascq;
            // 2/3A is the tray open or the disc out: nothing to retry.
            if (arc == ATAPI_ECHECK && atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A) {
                __atomic_store_n(&gone_flag, 1u, __ATOMIC_RELEASE);
                rc = CD_EMEDIUM;
                break;
            }
            if (++tries > TRACK_RETRIES) { rc = CD_EDISCIO; break; }
            track_stat.retries++;
            atapi_wait_ready(2000);
            continue;
        }
        tries = 0;
        track_stat.chunks++;

        // Whole blocks go to their slot straight from the chunk; the block a
        // chunk boundary splits is carried over to the next one.
        const uint8_t *p = chunk + skip;
        uint32_t avail = n * DISC_SECTOR - skip;
        bool wrapped = false;
        skip = 0;
        while (avail) {
            if (carry_n == 0 && avail >= t.blk) {
                disc_slot_put(seq, p);
                p += t.blk; avail -= t.blk;
            } else {
                uint32_t take = t.blk - carry_n;
                if (take > avail) take = avail;
                memcpy(carry + carry_n, p, take);
                carry_n += take; p += take; avail -= take;
                if (carry_n < t.blk) break;
                disc_slot_put(seq, carry);
                carry_n = 0;
            }
            seq++; idx++;
            track_stat.blocks++;
            __atomic_store_n(&next_seq, seq, __ATOMIC_RELEASE);
            if (idx == t.nblocks) {
                if (!t.loop) { __atomic_store_n(&eos_flag, 1u, __ATOMIC_RELEASE); goto done; }
                seek_blk(loop.ls_blk, &lba, &skip);   // one seek per lap
                carry_n = 0; idx = loop.ls_blk;
                track_stat.laps++;
                wrapped = true;
                break;
            }
        }
        if (!wrapped) lba += n;
    }
done:
    track_stat.running = 0u;
    return rc;
}
