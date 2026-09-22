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
#define ATAPI_READ_TOC        0x43u
#define ATAPI_GET_EVENT_STATUS 0x4Au
#define ATAPI_MODE_SENSE10    0x5Au
#define ATAPI_SET_CD_SPEED    0xBBu
#define ATAPI_READ_CD         0xBEu

// READ CD byte 9: 0x10 is user data alone, 0xF8 asks for every field, and for
// CD-DA both come back as the same 2352 bytes. Some 1990s firmware takes only
// the second form for audio.
#define ATAPI_RCD_USER   0x10u
#define ATAPI_RCD_ALL    0xF8u
#define ATAPI_CDDA_BYTES 2352u

// READ TOC format 0 is a 4-byte header and one of these per track; 0xAA is
// the lead-out. Audio when bit 2 of ctrl is clear.
#define ATAPI_TOC_HDR    4u
#define ATAPI_TOC_DESC   8u
#define ATAPI_TOC_LEADOUT 0xAAu
#define ATAPI_TOC_MAX    (ATAPI_TOC_HDR + 100u * ATAPI_TOC_DESC)

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

// Door state from GET EVENT STATUS NOTIFICATION's media class (polled).
int atapi_tray_open(bool *open);

// The disc's table of contents, format 0, LBA addressing. Drives answer fewer
// bytes than asked for, so *got is the only size to trust.
int atapi_read_toc(void *buf, size_t len, size_t *got);

// nsec raw CD-DA sectors of ATAPI_CDDA_BYTES each. `flags` is byte 9.
int atapi_read_cd(uint32_t lba, uint32_t nsec, uint8_t flags, void *buf,
                  size_t maxlen, size_t *got);

// MODE SENSE(10) of the CD capabilities page (2A): an 8-byte mode header, then
// the page. Byte 5 of the page body: bit 0 CD-DA supported, bit 1 stream
// accurate.
int atapi_mode_sense_cap(void *buf, size_t len, size_t *got);

// Retry TEST UNIT READY while the drive reports "becoming ready" after a
// disc change or spin-up. Returns ATAPI_OK once the medium is usable.
int atapi_wait_ready(uint32_t timeout_ms);

#endif
