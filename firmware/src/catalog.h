// catalog.h - the NESCDISC disc: one sector at LBA 0 naming a ROM to boot and
// the tracks the game asks for by mailbox. No filesystem, by design - the
// catalog is one 2 KiB read at insert, and ISO9660 would cost a path-table
// walk for names nothing needs. LBA 1..15, the ISO system area, stays free so
// a hybrid disc remains possible.
#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdcore.h"

#define GAME_MAGIC        "NESCDISC"      // 8 bytes, no NUL
#define GAME_VER          2u
#define GAME_ITEMS_MAX    32u
#define GAME_ITEM_LBA_MIN 16u
#define GAME_TITLE        32u

// Item types. Type 2 is reserved for items this build does not serve; they are
// listed and refused, never treated as a malformed catalog.
enum { ITEM_ROM = 1u, ITEM_RESERVED = 2u, ITEM_TRACK = 3u };

// Little-endian and naturally aligned: a cast over the sector buffer.
typedef struct {
    uint32_t type;
    uint32_t id;                     // the mailbox value that names it; ROM is 0
    uint32_t lba;                    // absolute, the item's first sector
    uint32_t sectors;
    uint32_t crc32;                  // over the padded item, 0 = not carried
    uint32_t reserved[3];
} cat_item_t;

typedef struct {
    char     magic[8];
    uint32_t version;
    char     title[GAME_TITLE];      // NUL-terminated
    uint32_t nitems;
    uint32_t reserved[4];
    cat_item_t item[GAME_ITEMS_MAX];
} cat_hdr_t;

// Pure, so the same rules run off-target: magic, version, one ROM, ids unique
// per type, items ascending from LBA 16 and non-overlapping.
int catalog_check(const cat_hdr_t *h);

// Read and check LBA 0. CD_ENOGAME when the sector holds something else.
int  catalog_open(void);
void catalog_close(void);
bool catalog_is_open(void);
const cat_hdr_t  *catalog_hdr(void);
const cat_item_t *catalog_find(uint32_t type, uint32_t id);   // NULL if absent
const cat_item_t *catalog_first(uint32_t type);               // lowest id
const cat_item_t *catalog_next(uint32_t type, uint32_t id);   // lowest id > id
const char *catalog_type_name(uint32_t type);

// The ROM item whole into dst, a sector multiple, checked against its crc when
// it carries one. Core0, with the bus idle: it borrows the producer's buffer.
int catalog_stage_rom(const cat_item_t *it, uint8_t *dst, size_t cap, size_t *len);

// Read an item back and check its crc; tick may abort.
typedef bool (*catalog_tick_fn)(uint32_t done, uint32_t total);
int catalog_verify_item(const cat_item_t *it, catalog_tick_fn tick);

#endif
