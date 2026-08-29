// host_game.c - run the catalog readers against a mastered game disc on the
// PC: the catalog rules, the ROM staged for n8push, and a track streamed
// through the slot ring.
//
//   gcc -O2 -I src -I test/stub -o /tmp/host_game test/host_game.c
//       src/catalog.c src/track.c src/disc.c src/n8push.c src/cdcore.c  (one line)
//   /tmp/host_game GAME.img [--track N=track.rom]
//
// The standalone .rom is what mkgamedisc.py embedded: the reader must find
// exactly those bytes at the item's LBA.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atapi.h"
#include "bgmloop.h"
#include "catalog.h"
#include "cdcore.h"
#include "disc.h"
#include "edn8.h"
#include "n8push.h"
#include "track.h"

// -- atapi stub over the image file ------------------------------------------

static FILE *img;
static uint32_t reads, fail_every, gone_at;

uint8_t atapi_sense_key, atapi_sense_asc, atapi_sense_ascq;

int atapi_wait_ready(uint32_t timeout_ms) { (void)timeout_ms; return ATAPI_OK; }
int atapi_set_cd_speed(uint16_t kb) { (void)kb; return ATAPI_OK; }
int atapi_test_unit_ready(void) { return ATAPI_OK; }

int atapi_read10(uint32_t lba, uint16_t blocks, void *buf, size_t maxlen, size_t *got)
{
    size_t want = (size_t)blocks * DISC_SECTOR;
    if (want > maxlen) want = maxlen;
    reads++;
    if (gone_at && reads > gone_at) {
        atapi_sense_key = 2; atapi_sense_asc = 0x3A; atapi_sense_ascq = 0;
        *got = 0;
        return ATAPI_ECHECK;
    }
    if (fail_every && reads % fail_every == 0) {
        atapi_sense_key = 3; atapi_sense_asc = 0x02; atapi_sense_ascq = 0x01;
        *got = 0;
        return ATAPI_ECHECK;
    }
    if (fseek(img, (long)lba * DISC_SECTOR, SEEK_SET) != 0) return ATAPI_ECHECK;
    *got = fread(buf, 1, want, img);
    return *got == want ? ATAPI_OK : ATAPI_ECHECK;
}

static uint32_t now_ms(void) { return 0; }

// -- edn8 stub: a transcript, as host_push.c keeps -----------------------------

uint8_t  edn8_last_status;
size_t   edn8_last_short;
uint32_t edn8_bytes_owed;
static char log_buf[4096];

static void logf_(const char *s)
{
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof(log_buf) - n, "%s", s);
}

const char *edn8_strerror(int rc) { (void)rc; return "stub error"; }
int edn8_dir_make(const char *path) { (void)path; return EDN8_OK; }
int edn8_make_path(const char *path) { (void)path; return EDN8_OK; }
int edn8_file_open(const char *path, uint8_t mode) { (void)path; (void)mode; logf_("open "); return EDN8_OK; }
int edn8_file_write(const void *src, size_t len) { (void)src; (void)len; logf_("write "); return EDN8_OK; }
int edn8_file_close(void) { logf_("close "); return EDN8_OK; }
int edn8_menu_test(void) { logf_("menu_test "); return EDN8_OK; }
int edn8_menu_install(const char *path, uint16_t *idx) { (void)path; if (idx) *idx = 0; logf_("install "); return EDN8_OK; }
int edn8_menu_start(void) { logf_("start "); return EDN8_OK; }
int edn8_mem_wr(uint32_t addr, const void *src, size_t len) { (void)addr; (void)src; (void)len; logf_("seed "); return EDN8_OK; }

// -- checks --------------------------------------------------------------------

static int fails;
static void check(int cond, const char *what)
{
    printf("  %-60s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)len);
    if (fread(p, 1, (size_t)len, f) != (size_t)len) { perror(path); exit(2); }
    fclose(f);
    *n = (size_t)len;
    return p;
}

// -- catalog rules on a hand-corrupted copy ---------------------------------

static void catalog_rules(void)
{
    const cat_hdr_t *h = catalog_hdr();
    cat_hdr_t c;

    c = *h; c.magic[0] = 'X';
    check(catalog_check(&c) == CD_ENOGAME, "catalog: bad magic refused");
    c = *h; c.version = GAME_VER + 1u;
    check(catalog_check(&c) == CD_EVERSION, "catalog: wrong version refused");
    c = *h; c.nitems = GAME_ITEMS_MAX + 1u;
    check(catalog_check(&c) == CD_EGAMEFMT, "catalog: too many items refused");
    c = *h; c.item[0].lba = GAME_ITEM_LBA_MIN - 1u;
    check(catalog_check(&c) == CD_EGAMEFMT, "catalog: item below LBA 16 refused");
    if (h->nitems > 1) {
        c = *h; c.item[1].lba = c.item[0].lba + c.item[0].sectors - 1u;
        check(catalog_check(&c) == CD_EGAMEFMT, "catalog: overlapping items refused");
        c = *h; c.item[1].type = ITEM_TRACK; c.item[1].id = 9;
        c.item[2] = c.item[1]; c.item[2].lba = c.item[1].lba + c.item[1].sectors;
        if (h->nitems > 2)
            check(catalog_check(&c) == CD_EGAMEFMT, "catalog: duplicate (track, id) refused");
    }
    c = *h; c.item[0].type = 7;
    check(catalog_check(&c) == CD_EGAMEFMT, "catalog: unknown type refused");
    c = *h; memset(c.title, 'T', GAME_TITLE);
    check(catalog_check(&c) == CD_EGAMEFMT, "catalog: unterminated title refused");
    check(catalog_check(h) == CD_OK, "catalog: the disc's own passes");

    // A disc carrying reserved items still has to open: they are listed and
    // refused at the mailbox, never treated as a broken catalog.
    if (h->nitems > 2) {
        c = *h;
        c.item[1].type = c.item[2].type = ITEM_RESERVED;
        c.item[1].id = c.item[2].id = 1;
        check(catalog_check(&c) == CD_OK, "catalog: repeated reserved ids accepted");
    }
}

// -- rom: stage + n8push -----------------------------------------------------

static void rom_checks(const cat_item_t *it)
{
    uint8_t *buf = malloc(4u << 20);
    size_t len = 0;
    int rc = catalog_stage_rom(it, buf, 4u << 20, &len);
    check(rc == CD_OK && len == (size_t)it->sectors * DISC_SECTOR, "rom: staged whole, crc ok");
    check(catalog_stage_rom(it, buf, 1024, &len) == CD_EROMBIG, "rom: too small a window refused");

    rc = n8push_open_at(buf, len, true);
    check(rc == CD_OK, "rom: n8push_open_at over the staged bytes");
    if (rc == CD_OK) {
        const n8push_hdr_t *h = n8push_hdr();
        check(h->boot[0] != 0, "rom: names a boot target");
        log_buf[0] = 0;
        rc = n8push_run();
        check(rc == CD_OK, "rom: n8push_run over the staged bytes");
        check(strncmp(log_buf, "menu_test ", 10) == 0, "rom: the menu is tested before any file");
        check(strstr(log_buf, "install seed start") != NULL,
              "rom: installed, mailbox seeded, then started");
    }
    buf[len - 1] ^= 0xFF;
    cat_item_t bad = *it;
    check(catalog_verify_item(&bad, NULL) == CD_OK, "rom: verify_item re-reads the disc, not the copy");
    free(buf);
}

// -- track: the producer through the slot ring -------------------------------

static const uint8_t *trk_ref;
static size_t trk_ref_n;
static uint32_t cons_seq, trk_wrong, trk_checked;
static uint32_t trk_stop_at;
static bgm_loop_t trk_loop;

static uint32_t consumer(void) { return cons_seq; }

// One block per idle call, checked against the standalone .rom's bytes.
static void consume_one(void)
{
    const track_t *t = track_info();
    if (track_next_seq() <= cons_seq) return;
    const uint8_t *p; uint32_t n;
    if (!disc_slot_get(cons_seq, &p, &n)) { trk_wrong++; }
    else if (trk_ref) {
        size_t at = TRACK_DATA + (size_t)bgm_loop_block(&trk_loop, cons_seq) * t->blk;
        if (at + n > trk_ref_n || memcmp(p, trk_ref + at, n) != 0) trk_wrong++;
        else trk_checked++;
    }
    if (track_next_seq() > cons_seq + TRACK_DEPTH + 64u) { printf("  producer ran ahead\n"); fails++; }
    cons_seq++;
    if (trk_stop_at && cons_seq >= trk_stop_at) track_stop();
}

static int track_run(uint32_t start, uint32_t stop_at)
{
    cons_seq = start; trk_stop_at = stop_at;
    track_reset();
    int rc = track_fill(start);
    while (track_next_seq() > cons_seq && !track_medium_gone()) consume_one();
    return rc;
}

static void track_checks(const cat_item_t *it, const char *ref)
{
    char what[96];
    size_t n = 0;
    trk_ref = ref ? slurp(ref, &n) : NULL;
    trk_ref_n = n;

    snprintf(what, sizeof what, "track %u: track_open(%u, %u)", it->id, it->lba, it->sectors);
    int rc = track_open(it->lba, it->sectors);
    check(rc == CD_OK, what);
    if (rc != CD_OK) return;
    const track_t *t = track_info();
    bgm_loop_init(&trk_loop, t->loop_start, t->loop_end, t->spr, t->nblocks);
    printf("    %u blocks of %u B, %s", t->nblocks, t->blk, t->loop ? "loop" : "once");
    if (t->loop_end) printf(", blocks %u..%u", trk_loop.ls_blk, trk_loop.le_blk);
    printf("\n");

    disc_idle = consume_one;
    track_consumer = consumer;
    trk_wrong = trk_checked = 0;

    // Across the loop seam, or to the end of a one-shot. Two seams on a range:
    // an off-by-one in the wrap target only shows once lap 2 re-enters it.
    uint32_t lap = trk_loop.le_blk - trk_loop.ls_blk + 1u;
    uint32_t span = t->nblocks + (t->loop_end ? 2u * lap : t->nblocks / 2u);
    rc = track_run(0, t->loop ? span : 0);
    check(rc == CD_OK, "track: fill from block 0");
    check(trk_wrong == 0, "track: every block == the standalone .rom's bytes");
    if (t->loop) check(track_stat.laps >= (t->loop_end ? 2u : 1u), "track: wrapped");
    else         check(track_eos() && cons_seq == t->nblocks, "track: one-shot read out and ended");
    printf("    %u blocks checked, %u chunks, %u laps\n", trk_checked, track_stat.chunks, track_stat.laps);

    // Restart at the three residues of a 740 B block inside a sector.
    for (uint32_t s = 0; s < 3; s++) {
        uint32_t start = t->nblocks / 3u + s;
        if (start >= t->nblocks) break;
        trk_wrong = 0;
        rc = track_run(start, start + 40u);
        snprintf(what, sizeof what, "track: restart at block %u lands on block %u", start, start);
        check(rc == CD_OK && trk_wrong == 0, what);
    }

    // The tray opens: one read, CD_EMEDIUM, nothing more.
    reads = 0; gone_at = 2;
    rc = track_run(0, 0);
    check(rc == CD_EMEDIUM && track_medium_gone(), "track: tray open -> CD_EMEDIUM");
    check(reads == gone_at + 1u, "track: exactly one read after the medium went");
    gone_at = 0;

    // Every other read fails: each is retried once and the track still plays.
    reads = 0; fail_every = 2;
    trk_wrong = 0;
    rc = track_run(0, t->loop ? t->nblocks : 0);
    check(rc == CD_OK && track_stat.retries > 0 && trk_wrong == 0,
          "track: read errors are retried and the blocks still match");
    fail_every = 0;

    disc_idle = NULL;
    track_consumer = NULL;
    if (trk_ref) free((void *)trk_ref);
    trk_ref = NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: host_game GAME.img [--track N=ROM]\n");
        return 2;
    }
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    const char *track_ref[GAME_ITEMS_MAX] = { 0 };
    for (int i = 2; i + 1 < argc; i += 2) {
        unsigned id = (unsigned)atoi(argv[i + 1]);
        const char *eq = strchr(argv[i + 1], '=');
        if (!eq || id >= GAME_ITEMS_MAX) continue;
        if (!strcmp(argv[i], "--track")) track_ref[id] = eq + 1;
    }

    disc_now_ms = now_ms;
    static uint8_t aring[DISC_SLOTS_MAX * DISC_BLOCK_MAX];
    disc_slots_attach(aring, sizeof aring);

    int rc = catalog_open();
    printf("catalog_open -> %d\n", rc);
    if (rc != CD_OK) return 1;
    const cat_hdr_t *h = catalog_hdr();
    printf("  \"%s\", %u items\n", h->title, h->nitems);
    for (uint32_t i = 0; i < h->nitems; i++)
        printf("  %-6s %2u  lba %6u  %5u sectors  crc %08X\n",
               catalog_type_name(h->item[i].type), h->item[i].id,
               h->item[i].lba, h->item[i].sectors, h->item[i].crc32);

    catalog_rules();
    check(catalog_find(ITEM_ROM, 0) != NULL, "find: the rom");
    check(catalog_find(ITEM_TRACK, 99) == NULL, "find: a missing track is NULL");
    if (catalog_first(ITEM_TRACK)) {
        uint32_t t0 = catalog_first(ITEM_TRACK)->id;
        const cat_item_t *nx = catalog_next(ITEM_TRACK, t0);
        check(!nx || nx->id > t0, "next: the track after the first has a higher id");
        check(catalog_next(ITEM_TRACK, 99) == NULL, "next: nothing past the last track");
    }

    rom_checks(catalog_find(ITEM_ROM, 0));
    for (uint32_t i = 0; i < h->nitems; i++) {
        const cat_item_t *it = &h->item[i];
        if (it->type == ITEM_TRACK
            && (it->id == catalog_first(ITEM_TRACK)->id || track_ref[it->id]))
            track_checks(it, track_ref[it->id]);
    }
    check(catalog_is_open(), "catalog still open after the item opens");

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
