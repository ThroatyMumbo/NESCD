// n8push.c - the N8PUSH1 container, and the one pass it buys on the menu.
//
// Every file, then install, then start.

#include <stdio.h>
#include <string.h>

#include "cdcore.h"
#include "edn8.h"
#include "mailbox.h"
#include "n8push.h"

#define CHUNK (1u << 18)            // one F_FWR, matching the cart's file protocol

static const uint8_t  *base;
static const n8push_hdr_t *hdr;

static bool path_ok(const char *p, size_t n)
{
    if (p[0] == 0 || p[0] == '/') return false;
    for (size_t i = 0; i < n; i++)
        if (p[i] == 0) return true;
    return false;                   // never terminated inside its field
}

int n8push_open_at(const void *b, size_t limit, bool check_crc)
{
    const n8push_hdr_t *h = b;

    base = b;
    hdr  = NULL;

    if (limit < sizeof(*h)) return CD_ESHAPE;
    if (memcmp(h->magic, N8PUSH_MAGIC, 8) != 0) return CD_ENOIMAGE;
    if (h->version != N8PUSH_VERSION) return CD_EVERSION;
    if (h->nfiles == 0 || h->nfiles > N8PUSH_MAXFILE) return CD_ESHAPE;

    if (h->payload_off < sizeof(*h)) return CD_ESHAPE;
    if ((uint64_t)h->payload_off + h->payload_bytes > limit) return CD_ESHAPE;
    if (h->boot[0] && !path_ok(h->boot, sizeof(h->boot))) return CD_ESHAPE;

    for (uint32_t i = 0; i < h->nfiles; i++) {
        const n8push_file_t *f = &h->file[i];
        if (f->len == 0) return CD_ESHAPE;
        if ((uint64_t)f->off + f->len > h->payload_bytes) return CD_ESHAPE;
        if (!path_ok(f->path, sizeof(f->path))) return CD_ESHAPE;
    }

    if (check_crc &&
        cd_crc32((const uint8_t *)b + h->payload_off, h->payload_bytes) != h->crc32)
        return CD_ECRC;

    hdr = h;
    return CD_OK;
}

int n8push_open(bool check_crc)
{
    return n8push_open_at((const void *)N8PUSH_BASE, N8PUSH_LIMIT, check_crc);
}

const n8push_hdr_t *n8push_hdr(void) { return hdr; }

const uint8_t *n8push_data(const n8push_file_t *f)
{
    return base + hdr->payload_off + f->off;
}

static int put_file(const n8push_file_t *f)
{
    int rc = edn8_make_path(f->path);
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_file_open(f->path, EDN8_FA_WRITE | EDN8_FA_CREATE_ALWAYS)) != EDN8_OK)
        return rc;

    const uint8_t *p = n8push_data(f);
    for (uint32_t off = 0; off < f->len; ) {
        uint32_t n = f->len - off;
        if (n > CHUNK) n = CHUNK;
        if ((rc = edn8_file_write(p + off, n)) != EDN8_OK) {
            edn8_file_close();
            return rc;
        }
        off += n;
    }
    return edn8_file_close();
}

int n8push_run(void)
{
    if (!hdr) return CD_ENOIMAGE;

    // The cart leaves the menu the moment a loader boots, and there is no
    // host-side reset, so check before spending a single file on it.
    if (edn8_menu_test() != EDN8_OK) return CD_EMENU;

    for (uint32_t i = 0; i < hdr->nfiles; i++) {
        const n8push_file_t *f = &hdr->file[i];
        printf("  %s  %lu B\n", f->path, (unsigned long)f->len);
        int rc = put_file(f);
        if (rc != EDN8_OK) return rc;
    }

    if (!hdr->boot[0]) return CD_OK;

    uint16_t idx = 0;
    int rc = edn8_menu_install(hdr->boot, &idx);
    if (rc != EDN8_OK) return rc;
    printf("  installed %s, mapper index %u\n", hdr->boot, idx);

    // CHR RAM comes up undefined and the ROM does not clear the mailbox until
    // it has run its own tile upload, so seed it here: the first poll lands
    // well inside that window and would read the garbage as a request.
    static const uint8_t zero[4] = {0};
    edn8_mem_wr(EDN8_ADDR_CHR + MAILBOX_ADDR, zero, sizeof zero);

    return edn8_menu_start();
}
