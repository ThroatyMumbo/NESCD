// disc.c - drive I/O and the PSRAM audio slots. No container knowledge lives
// here: catalog.c and track.c parse, this reads sectors and holds blocks.

#include <string.h>

#include "atapi.h"
#include "disc.h"

uint32_t (*disc_now_ms)(void);
void     (*disc_idle)(void);
void     (*disc_pause_ms)(uint32_t ms);

static bool speed_set;
static uint8_t scratch[DISC_SECTOR] __attribute__((aligned(4)));

uint32_t disc_fail_lba, disc_fail_tries;
uint32_t disc_fail_sense[DISC_READ_TRIES];

int disc_read_sectors(uint32_t lba, uint32_t nsec, void *buf)
{
    size_t want = (size_t)nsec * DISC_SECTOR;
    for (uint32_t t = 0; t < DISC_READ_TRIES; t++) {
        size_t got = 0;
        int arc = atapi_read10(lba, (uint16_t)nsec, buf, want, &got);
        if (arc == ATAPI_OK && got == want) return CD_OK;
        disc_fail_sense[t] = (arc == ATAPI_ECHECK)
            ? ((uint32_t)atapi_sense_key << 16 | (uint32_t)atapi_sense_asc << 8
               | atapi_sense_ascq)
            : 0xFFFFFFFFu;
        atapi_wait_ready(5000);
    }
    disc_fail_lba = lba;
    disc_fail_tries = DISC_READ_TRIES;
    return CD_EDISCIO;
}

// SET CD SPEED answers ok and then refuses reads (5/64/00) for a few seconds
// while TEST UNIT READY still says ready, so the window is waited out on a
// read of the sector the caller wants next. Core0 only, and the harnesses
// leave the hook NULL.
#define SPEED_SETTLE_TRIES 40u
#define SPEED_SETTLE_MS    250u

uint32_t disc_settle_tries;

int disc_prepare_at(uint32_t lba)
{
    int rc = atapi_wait_ready(30000);   // a cold drive needs its spin-up
    if (rc != ATAPI_OK) return CD_EDISCIO;
    if (speed_set) return CD_OK;
    atapi_set_cd_speed(DISC_READ_SPEED_X * ATAPI_SPEED_1X);
    speed_set = true;
    disc_settle_tries = 0;
    if (!disc_pause_ms) return CD_OK;
    for (uint32_t t = 0; t < SPEED_SETTLE_TRIES; t++) {
        size_t got = 0;
        disc_settle_tries++;
        if (atapi_read10(lba, 1, scratch, DISC_SECTOR, &got) == ATAPI_OK && got == DISC_SECTOR)
            break;
        disc_pause_ms(SPEED_SETTLE_MS);
    }
    return CD_OK;
}

int disc_prepare(void) { return disc_prepare_at(0); }

void disc_speed_reset(void) { speed_set = false; }

static uint8_t chunk[DISC_CHUNK_SECTORS * DISC_SECTOR] __attribute__((aligned(4)));

uint8_t *disc_chunk_buf(void) { return chunk; }

bool disc_heartbeat(uint32_t *parked_at)
{
    if (!disc_now_ms) return true;
    uint32_t now = disc_now_ms();
    if (now - *parked_at < DISC_IDLE_TUR_MS) return true;
    *parked_at = now;
    int rc = atapi_test_unit_ready();
    return !(rc == ATAPI_ECHECK && atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A);
}

// -- audio slots -----------------------------------------------------------

static uint8_t  *aring_base;
static uint32_t  aring_bytes, aslots, ablock;
static uint32_t  aseq[DISC_SLOTS_MAX];     // SRAM tags; ASEQ_NONE is empty

#define ASEQ_NONE 0xFFFFFFFFu

void disc_slots_attach(uint8_t *base, uint32_t bytes)
{
    aring_base = base;
    aring_bytes = bytes;
}

uint32_t disc_slot_count(void) { return aslots; }

int disc_slots_open(uint32_t block_bytes, uint32_t min_slots)
{
    if (!aring_base) return CD_EPSRAM;
    if (!block_bytes || block_bytes > DISC_BLOCK_MAX) return CD_ETRKFMT;
    uint32_t n = aring_bytes / block_bytes;
    if (n > DISC_SLOTS_MAX) n = DISC_SLOTS_MAX;
    if (n < min_slots) return CD_ESLOTS;
    ablock = block_bytes;
    aslots = n;
    for (uint32_t i = 0; i < DISC_SLOTS_MAX; i++) aseq[i] = ASEQ_NONE;
    return CD_OK;
}

void disc_slot_put(uint32_t seq, const uint8_t *block)
{
    uint32_t s = seq % aslots;
    __atomic_store_n(&aseq[s], ASEQ_NONE, __ATOMIC_RELAXED);
    memcpy(aring_base + (size_t)s * ablock, block, ablock);
    __atomic_store_n(&aseq[s], seq, __ATOMIC_RELEASE);
}

bool disc_slot_get(uint32_t seq, const uint8_t **p, uint32_t *n)
{
    if (!aslots) return false;
    uint32_t s = seq % aslots;
    if (__atomic_load_n(&aseq[s], __ATOMIC_ACQUIRE) != seq) return false;
    *p = aring_base + (size_t)s * ablock;
    *n = ablock;
    return true;
}
