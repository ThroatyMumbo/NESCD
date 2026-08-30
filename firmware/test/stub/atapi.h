// Host-test stand-in for the firmware's atapi.h: only what disc.c uses.
#ifndef ATAPI_H
#define ATAPI_H

#include <stddef.h>
#include <stdint.h>

#define ATAPI_OK        0
#define ATAPI_ETIMEOUT -1
#define ATAPI_ECHECK   -2
#define ATAPI_EPARAM   -3

extern uint8_t atapi_sense_key, atapi_sense_asc, atapi_sense_ascq;

int atapi_read10(uint32_t lba, uint16_t blocks, void *buf, size_t maxlen,
                 size_t *got);
int atapi_wait_ready(uint32_t timeout_ms);
int atapi_set_cd_speed(uint16_t read_kb_s);
int atapi_test_unit_ready(void);
#define ATAPI_SPEED_1X 176u

// cdda.c's side, stubbed over a synthetic disc by host_cdda.c.
#define ATAPI_RCD_USER    0x10u
#define ATAPI_RCD_ALL     0xF8u
#define ATAPI_CDDA_BYTES  2352u
#define ATAPI_TOC_HDR     4u
#define ATAPI_TOC_DESC    8u
#define ATAPI_TOC_LEADOUT 0xAAu
#define ATAPI_TOC_MAX     (ATAPI_TOC_HDR + 100u * ATAPI_TOC_DESC)
int atapi_read_toc(void *buf, size_t len, size_t *got);
int atapi_read_cd(uint32_t lba, uint32_t nsec, uint8_t flags, void *buf,
                  size_t maxlen, size_t *got);
int atapi_mode_sense_cap(void *buf, size_t len, size_t *got);

#endif
