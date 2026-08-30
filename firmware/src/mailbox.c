#include <string.h>

#include "cdcore.h"
#include "edn8.h"
#include "mailbox.h"

// The game writes a mailbox byte through $2007, so a poll can land between the
// two halves of that write: take the pair only when it reads twice the same.
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

int mailbox_rd(uint8_t v[2]) { return mailbox_rd_at(0x1FF8u, v, 2); }

int mailbox_wr_at(uint32_t ppu, const uint8_t *v, size_t n)
{
    return edn8_mem_wr(EDN8_ADDR_CHR + MAILBOX_HOST(ppu), v, n);
}

int mailbox_status_wr(uint8_t v)
{
    return edn8_mem_wr(EDN8_ADDR_CHR + MAILBOX_STATUS_ADDR, &v, 1);
}
