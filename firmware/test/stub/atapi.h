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

#endif
