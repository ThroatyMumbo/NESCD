// catalog.c - the NESCDISC catalog.

#include <string.h>

#include "catalog.h"
#include "disc.h"

_Static_assert(sizeof(cat_item_t) == 32, "item entry is 32 bytes");
_Static_assert(sizeof(cat_hdr_t) == 64 + 32 * GAME_ITEMS_MAX, "catalog fits sector 0");

static cat_hdr_t cat;
static bool opened;

const char *catalog_type_name(uint32_t type)
{
    switch (type) {
    case ITEM_ROM:      return "rom";
    case ITEM_RESERVED: return "reserved";
    case ITEM_TRACK:    return "track";
    default:         return "?";
    }
}

int catalog_check(const cat_hdr_t *h)
{
    if (memcmp(h->magic, GAME_MAGIC, 8) != 0) return CD_ENOGAME;
    if (h->version != GAME_VER) return CD_EVERSION;
    if (h->nitems == 0 || h->nitems > GAME_ITEMS_MAX) return CD_EGAMEFMT;
    if (!memchr(h->title, 0, GAME_TITLE)) return CD_EGAMEFMT;

    uint32_t roms = 0, end = GAME_ITEM_LBA_MIN;
    for (uint32_t i = 0; i < h->nitems; i++) {
        const cat_item_t *it = &h->item[i];
        if (it->type < ITEM_ROM || it->type > ITEM_TRACK) return CD_EGAMEFMT;
        if (it->sectors == 0 || it->lba < end) return CD_EGAMEFMT;
        if (it->lba + it->sectors < it->lba) return CD_EGAMEFMT;
        end = it->lba + it->sectors;
        if (it->type == ITEM_ROM) { if (it->id) return CD_EGAMEFMT; roms++; }
        else if (!it->id) return CD_EGAMEFMT;

        // Reserved ids may repeat, since nothing here interprets them. Rom and
        // track ids stay unique, and a repeat there is a malformed catalog.
        if (it->type != ITEM_RESERVED)
            for (uint32_t j = 0; j < i; j++)
                if (h->item[j].type == it->type && h->item[j].id == it->id)
                    return CD_EGAMEFMT;
    }
    if (roms != 1) return CD_EGAMEFMT;
    return CD_OK;
}

int catalog_open(void)
{
    opened = false;
    int rc = disc_prepare();
    if (rc != CD_OK) return rc;

    uint8_t *buf = disc_chunk_buf();
    if ((rc = disc_read_sectors(0, 1, buf)) != CD_OK) return rc;
    memcpy(&cat, buf, sizeof(cat));
    if ((rc = catalog_check(&cat)) != CD_OK) return rc;
    opened = true;
    return CD_OK;
}

void catalog_close(void) { opened = false; }
bool catalog_is_open(void) { return opened; }
const cat_hdr_t *catalog_hdr(void) { return opened ? &cat : NULL; }

const cat_item_t *catalog_find(uint32_t type, uint32_t id)
{
    if (!opened) return NULL;
    for (uint32_t i = 0; i < cat.nitems; i++)
        if (cat.item[i].type == type && cat.item[i].id == id) return &cat.item[i];
    return NULL;
}

const cat_item_t *catalog_next(uint32_t type, uint32_t id)
{
    const cat_item_t *best = NULL;
    if (!opened) return NULL;
    for (uint32_t i = 0; i < cat.nitems; i++)
        if (cat.item[i].type == type && cat.item[i].id > id
            && (!best || cat.item[i].id < best->id))
            best = &cat.item[i];
    return best;
}

const cat_item_t *catalog_first(uint32_t type)
{
    return catalog_next(type, 0);
}

// The item in DISC_CHUNK_SECTORS pieces through the SRAM buffer, crc'd on the
// way; dst may be NULL to only verify.
static int walk_item(const cat_item_t *it, uint8_t *dst, catalog_tick_fn tick)
{
    uint8_t *chunk = disc_chunk_buf();
    uint32_t c = 0xFFFFFFFFu, done = 0;
    while (done < it->sectors) {
        uint32_t n = it->sectors - done;
        if (n > DISC_CHUNK_SECTORS) n = DISC_CHUNK_SECTORS;
        int rc = disc_read_sectors(it->lba + done, n, chunk);
        if (rc != CD_OK) return rc;
        size_t bytes = (size_t)n * DISC_SECTOR;
        if (dst) memcpy(dst + (size_t)done * DISC_SECTOR, chunk, bytes);
        c = cd_crc32_step(c, chunk, bytes);
        done += n;
        if (tick && !tick(done, it->sectors)) return CD_EDISCIO;
    }
    if (it->crc32 && (~c) != it->crc32) return CD_ECRC;
    return CD_OK;
}

int catalog_stage_rom(const cat_item_t *it, uint8_t *dst, size_t cap, size_t *len)
{
    if (!opened || !it || it->type != ITEM_ROM) return CD_ENOITEM;
    size_t bytes = (size_t)it->sectors * DISC_SECTOR;
    if (bytes > cap) return CD_EROMBIG;
    int rc = walk_item(it, dst, NULL);
    if (rc != CD_OK) return rc;
    *len = bytes;
    return CD_OK;
}

int catalog_verify_item(const cat_item_t *it, catalog_tick_fn tick)
{
    if (!opened || !it) return CD_ENOITEM;
    return walk_item(it, NULL, tick);
}
