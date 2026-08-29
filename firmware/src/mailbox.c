#include "cdcore.h"
#include "edn8.h"
#include "mailbox.h"

// The game writes a mailbox byte through $2007, so a poll can land between the
// two halves of that write: take the pair only when it reads twice the same.
int mailbox_rd(uint8_t v[2])
{
    uint8_t a[2], b[2];
    int rc = edn8_mem_rd(EDN8_ADDR_CHR + MAILBOX_ADDR, a, 2);
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_mem_rd(EDN8_ADDR_CHR + MAILBOX_ADDR, b, 2)) != EDN8_OK)
        return rc;
    if (a[0] != b[0] || a[1] != b[1]) return CD_EMBOX;
    v[0] = a[0];
    v[1] = a[1];
    return CD_OK;
}

int mailbox_status_wr(uint8_t v)
{
    return edn8_mem_wr(EDN8_ADDR_CHR + MAILBOX_STATUS_ADDR, &v, 1);
}
