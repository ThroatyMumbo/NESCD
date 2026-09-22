// atapi.c - see atapi.h. PACKET command layer on top of the ata.c taskfile.

#include "atapi.h"
#include "ata.h"
#include <string.h>

uint8_t atapi_sense_key, atapi_sense_asc, atapi_sense_ascq;

// Per-DRQ chunk cap handed to the drive in the byte-count registers. Must be
// even; the drive may return less per burst but never more.
#define BCOUNT_MAX 0xFFFEu

const char *atapi_strerror(int rc)
{
    switch (rc) {
    case ATAPI_OK:       return "ok";
    case ATAPI_ETIMEOUT: return "timeout";
    case ATAPI_ECHECK:   return "check condition";
    case ATAPI_EPARAM:   return "bad parameter";
    default:             return "?";
    }
}

const char *atapi_sense_text(uint8_t key)
{
    static const char *k[16] = {
        "no sense", "recovered", "not ready", "medium error", "hardware error",
        "illegal request", "unit attention", "data protect", "blank check",
        "vendor", "copy aborted", "aborted command", "?", "volume overflow",
        "miscompare", "?"
    };
    return k[key & 0x0F];
}

int atapi_packet(const uint8_t cdb[ATAPI_CDB_LEN], void *buf, size_t maxlen,
                 size_t *got)
{
    if (got) *got = 0;
    if (!ata_wait_not_bsy(5000)) return ATAPI_ETIMEOUT;

    uint16_t bc = (maxlen == 0 || maxlen > BCOUNT_MAX) ? BCOUNT_MAX : (uint16_t)maxlen;
    bc &= ~1u;

    ata_select_device(0);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_FEATURES,  0);          // PIO, no overlap
    ata_reg_write8(ATA_CS_CMD, ATA_REG_BCOUNT_LO, bc & 0xFFu);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_BCOUNT_HI, (bc >> 8) & 0xFFu);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_COMMAND,   ATA_CMD_PACKET);

    if (!ata_wait_drq(5000)) return ATAPI_ETIMEOUT;

    uint16_t words[ATAPI_CDB_LEN / 2];
    memcpy(words, cdb, ATAPI_CDB_LEN);
    ata_write_data_burst(words, ATAPI_CDB_LEN / 2);

    // Data-in phase: the drive announces each burst's size in the byte-count
    // registers, and drops DRQ when the command is complete.
    size_t done = 0;
    for (;;) {
        if (!ata_wait_not_bsy(30000)) return ATAPI_ETIMEOUT;
        uint8_t st = ata_status();
        if (!(st & ATA_ST_DRQ)) {
            if (st & (ATA_ST_ERR | ATA_ST_DF)) break;
            if (got) *got = done;
            return ATAPI_OK;
        }
        size_t n = (size_t)ata_reg_read8(ATA_CS_CMD, ATA_REG_BCOUNT_LO)
                 | ((size_t)ata_reg_read8(ATA_CS_CMD, ATA_REG_BCOUNT_HI) << 8);
        if (n == 0) break;

        size_t take = (done + n > maxlen) ? (maxlen > done ? maxlen - done : 0) : n;
        take &= ~(size_t)1;                      // bursts move whole words
        if (take && buf) {
            // Let the DMA carry the burst so ata_wait_hook keeps running under
            // it; begin() declines an unaligned or odd request, and the
            // blocking burst is the same transfer without the overlap.
            uint16_t *p = (uint16_t *)((uint8_t *)buf + done);
            if (ata_read_begin(p, take / 2)) {
                while (!ata_read_poll()) if (ata_wait_hook) ata_wait_hook();
                ata_read_end();
            } else {
                ata_read_data_burst(p, take / 2);
            }
        }
        if (n > take) ata_skip_data((n - take) / 2);   // drain what we cannot store
        done += take;
    }

    if (got) *got = done;
    if (cdb[0] != ATAPI_REQUEST_SENSE) atapi_request_sense();
    return ATAPI_ECHECK;
}

int atapi_test_unit_ready(void)
{
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_TEST_UNIT_READY };
    return atapi_packet(cdb, NULL, 0, NULL);
}

int atapi_request_sense(void)
{
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    size_t got = 0;

    atapi_sense_key = atapi_sense_asc = atapi_sense_ascq = 0;
    memset(sense, 0, sizeof(sense));
    int rc = atapi_packet(cdb, sense, sizeof(sense), &got);
    if (got >= 14) {
        atapi_sense_key  = sense[2] & 0x0F;
        atapi_sense_asc  = sense[12];
        atapi_sense_ascq = sense[13];
    }
    return rc;
}

int atapi_inquiry(void *buf, size_t len, size_t *got)
{
    if (len > 255) len = 255;
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_INQUIRY, 0, 0, 0, (uint8_t)len, 0 };
    return atapi_packet(cdb, buf, len, got);
}

int atapi_start_stop(uint8_t mode)
{
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_START_STOP_UNIT, 0, 0, 0, mode, 0 };
    return atapi_packet(cdb, NULL, 0, NULL);
}

int atapi_read_capacity(uint32_t *last_lba, uint32_t *block_size)
{
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_READ_CAPACITY };
    uint8_t cap[8];
    size_t got = 0;

    int rc = atapi_packet(cdb, cap, sizeof(cap), &got);
    if (rc != ATAPI_OK || got < 8) return (rc == ATAPI_OK) ? ATAPI_ETIMEOUT : rc;

    if (last_lba)   *last_lba   = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16)
                                | ((uint32_t)cap[2] << 8)  |  (uint32_t)cap[3];
    if (block_size) *block_size = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16)
                                | ((uint32_t)cap[6] << 8)  |  (uint32_t)cap[7];
    return ATAPI_OK;
}

int atapi_read10(uint32_t lba, uint16_t blocks, void *buf, size_t maxlen,
                 size_t *got)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_READ10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0,
        (uint8_t)(blocks >> 8), (uint8_t)blocks,
        0, 0, 0
    };
    return atapi_packet(cdb, buf, maxlen, got);
}

int atapi_set_cd_speed(uint16_t read_kb_s)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_SET_CD_SPEED, 0,
        (uint8_t)(read_kb_s >> 8), (uint8_t)read_kb_s,
        0xFF, 0xFF,                       // leave write speed at maximum
        0, 0, 0, 0, 0, 0
    };
    return atapi_packet(cdb, NULL, 0, NULL);
}

int atapi_read_toc(void *buf, size_t len, size_t *got)
{
    if (len > 0xFFFF) len = 0xFFFF;
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_READ_TOC, 0, 0, 0, 0, 0,
        0,                                // starting track
        (uint8_t)(len >> 8), (uint8_t)len,
        0, 0, 0
    };
    return atapi_packet(cdb, buf, len, got);
}

int atapi_read_cd(uint32_t lba, uint32_t nsec, uint8_t flags, void *buf,
                  size_t maxlen, size_t *got)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_READ_CD, 0x04,              // expected sector type: CD-DA
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        (uint8_t)(nsec >> 16), (uint8_t)(nsec >> 8), (uint8_t)nsec,
        flags, 0, 0
    };
    return atapi_packet(cdb, buf, maxlen, got);
}

int atapi_mode_sense_cap(void *buf, size_t len, size_t *got)
{
    if (len > 0xFFFF) len = 0xFFFF;
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_MODE_SENSE10, 0, 0x2A, 0, 0, 0, 0,
        (uint8_t)(len >> 8), (uint8_t)len,
        0, 0, 0
    };
    return atapi_packet(cdb, buf, len, got);
}

int atapi_tray_open(bool *open)
{
    uint8_t cdb[ATAPI_CDB_LEN] = { ATAPI_GET_EVENT_STATUS, 0x01, 0, 0, 0x10, 0, 0, 0, 8, 0, 0, 0 };
    uint8_t ev[8];
    size_t got = 0;

    int rc = atapi_packet(cdb, ev, sizeof(ev), &got);
    if (rc != ATAPI_OK) return rc;
    // NEA set or a class other than media (4) means the drive has no answer.
    if (got < 6 || (ev[2] & 0x80) || (ev[2] & 0x07) != 4) return ATAPI_EPARAM;
    *open = ev[5] & 0x01;
    return ATAPI_OK;
}

int atapi_wait_ready(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    for (;;) {
        int rc = atapi_test_unit_ready();
        if (rc == ATAPI_OK) return ATAPI_OK;
        if (rc == ATAPI_ECHECK) {
            // 2/04/xx = becoming ready; 6/28/00 = medium changed. Both retry.
            bool transient = (atapi_sense_key == 0x02 && atapi_sense_asc == 0x04)
                          || (atapi_sense_key == 0x06);
            if (!transient) return rc;
        } else if (rc != ATAPI_ETIMEOUT) {
            return rc;
        }
        if (time_reached(end)) return ATAPI_ETIMEOUT;
        sleep_ms(250);
    }
}
