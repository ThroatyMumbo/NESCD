#include <string.h>

#include "cdcore.h"
#include "edn8.h"
#include "mailbox.h"

// The ROM writes its page through $2007, so a poll can land between two of
// those writes: take the bytes only when they read twice the same.
int mailbox_rd_at(uint32_t ppu, uint8_t *v, size_t n)
{
    uint8_t a[16], b[16];
    if (n > sizeof(a)) return CD_EMBOX;
    int rc = edn8_mem_rd(EDN8_ADDR_CHR + MAILBOX_HOST(ppu), a, n);
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_mem_rd(EDN8_ADDR_CHR + MAILBOX_HOST(ppu), b, n)) != EDN8_OK)
        return rc;
    if (memcmp(a, b, n) != 0) return CD_EMBOX;
    memcpy(v, a, n);
    return CD_OK;
}

static uint32_t base = MAILBOX_PPU;

void mailbox_at(uint32_t ppu) { base = ppu; }

uint32_t mailbox_where(void) { return base; }

int mailbox_rd(uint8_t *v)
{
    int rc = edn8_mem_rd(EDN8_ADDR_CHR + MAILBOX_HOST(base), v, 1);
    return rc == EDN8_OK ? CD_OK : rc;
}

int mailbox_wr_at(uint32_t ppu, const uint8_t *v, size_t n)
{
    return edn8_mem_wr(EDN8_ADDR_CHR + MAILBOX_HOST(ppu), v, n);
}
