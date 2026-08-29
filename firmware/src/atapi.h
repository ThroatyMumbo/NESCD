// atapi.h - ATAPI PACKET layer over the ata.c taskfile.
//
// Every ATAPI command is a 12-byte SCSI CDB handed to the drive after the
// PACKET (0xA0) command. Opcodes and field layouts follow INF-8020i / SFF-8020i.

#ifndef ATAPI_H
#define ATAPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ATAPI_CDB_LEN 12

// ---- opcodes ------------------------------------------------------------
#define ATAPI_TEST_UNIT_READY 0x00u
#define ATAPI_REQUEST_SENSE   0x03u
#define ATAPI_INQUIRY         0x12u
#define ATAPI_START_STOP_UNIT 0x1Bu
#define ATAPI_READ_CAPACITY   0x25u
#define ATAPI_READ10          0x28u
#define ATAPI_SET_CD_SPEED    0xBBu

// START/STOP UNIT byte 4: bit0 = Start, bit1 = LoEj.
#define ATAPI_SS_STOP  0x00u
#define ATAPI_SS_START 0x01u
#define ATAPI_SS_EJECT 0x02u
#define ATAPI_SS_LOAD  0x03u

// 1x CD is 176 KB/s in SET CD SPEED's units.
#define ATAPI_SPEED_1X 176u

// ---- results ------------------------------------------------------------
#define ATAPI_OK        0
#define ATAPI_ETIMEOUT -1
#define ATAPI_ECHECK   -2      // drive raised ERR; sense data is available
#define ATAPI_EPARAM   -3

// Last sense data from a failed command: key, ASC, ASCQ.
extern uint8_t atapi_sense_key, atapi_sense_asc, atapi_sense_ascq;

const char *atapi_strerror(int rc);
const char *atapi_sense_text(uint8_t key);

// Core: issue a CDB, then read up to maxlen bytes of the data-in phase.
// Pass buf=NULL/maxlen=0 for non-data commands. *got receives the byte count.
int atapi_packet(const uint8_t cdb[ATAPI_CDB_LEN], void *buf, size_t maxlen,
                 size_t *got);

int atapi_test_unit_ready(void);
int atapi_request_sense(void);
int atapi_inquiry(void *buf, size_t len, size_t *got);
int atapi_start_stop(uint8_t mode);
int atapi_read_capacity(uint32_t *last_lba, uint32_t *block_size);
int atapi_read10(uint32_t lba, uint16_t blocks, void *buf, size_t maxlen,
                 size_t *got);
int atapi_set_cd_speed(uint16_t read_kb_s);

// Retry TEST UNIT READY while the drive reports "becoming ready" after a
// disc change or spin-up. Returns ATAPI_OK once the medium is usable.
int atapi_wait_ready(uint32_t timeout_ms);

#endif
