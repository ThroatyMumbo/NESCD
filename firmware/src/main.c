// main.c - the console. An ATAPI CD-ROM on one side, an EverDrive N8 Pro on
// the other, and a PCM5102 hanging off the NES expansion port.
//
// Connect with picocom on the debugprobe's if01 CDC (115200 8N1); h lists the
// commands. Bring-up order: 'i' proves the ATA wiring with no disc in the tray,
// 'e'/'l' proves the CDB path, then 'c'/'d'/'b' exercise the read path.
//
// Between commands the console polls the tray, the cart and the game's mailbox,
// so inserting a disc boots its game and plays its music with no command at
// all. Only a tray edge counts as an insertion, and a disc already in at
// power-on is one.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/regs/usb.h"
#include "hardware/xip_cache.h"
#include "ata.h"
#include "atapi.h"
#include "audio.h"
#include "bgm.h"
#include "catalog.h"
#include "cdcore.h"
#include "cdda.h"
#include "diag.h"
#include "disc.h"
#include "disctask.h"
#include "edn8.h"
#include "eject.h"
#include "jingle.h"
#include "tone.h"
#include "knob.h"
#include "mailbox.h"
#include "n8push.h"
#include "player.h"
#include "psram.h"
#include "track.h"
#include "usb_link.h"

#define SECTOR_BYTES  2048u
#define BULK_SECTORS  16u                     // sectors per READ(10) in 'b'
#define RUN_LED_PIN   42u                     // panel red LED: steady = cart linked, 2 Hz blink = no cart
#define RUN_LED_MS    250u

static repeating_timer_t run_led_timer;

static bool __not_in_flash_func(run_led_tick)(repeating_timer_t *t)
{
    (void)t;
    gpio_put(RUN_LED_PIN, usb_link_state()->mounted || !gpio_get_out_level(RUN_LED_PIN));
    return true;
}

// Word-aligned so the burst reads take the DMA path: DMA_SIZE_32 ignores the
// low address bits, so ata_read_data_burst() falls back to a CPU pop loop for
// anything at an odd word offset.
static uint16_t identify[256]                       __attribute__((aligned(4)));
static uint8_t  sector[SECTOR_BYTES]                __attribute__((aligned(4)));
static uint8_t  bulk[BULK_SECTORS * SECTOR_BYTES]   __attribute__((aligned(4)));

static bool autoplay_on = true;      // boot a disc as it is inserted

static void hexdump(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i += 16) {
        printf("%04x: ", i);
        for (int j = 0; j < 16 && i + j < n; j++) printf("%02x ", p[i + j]);
        printf(" |");
        for (int j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
}

static void help(void)
{
    printf("\nnescd - ATAPI host on PGA2350\n"
           "  h            help\n"
           "  x            hard reset + IDENTIFY\n"
           "  i            IDENTIFY PACKET DEVICE\n"
           "  q            INQUIRY\n"
           "  u            TEST UNIT READY (waits for spin-up)\n"
           "  e | l        eject / load tray\n"
           "  k <n>        SET CD SPEED to n x (0 = max)\n"
           "  c            READ CAPACITY\n"
           "  d <lba>      READ(10) one sector + hexdump\n"
           "  b <lba> <n>  timed bulk read of n sectors\n"
           "  r <hex...>   raw CDB (up to 12 bytes)\n"
           "  t <0..2>     ATA PIO timing mode (now %u, %lu ns cycle)\n"
           "  A <lba>      async sector read, counts USB pumps during it\n"
           "  R            force the bus idle, restart the PIO machines\n"
           "everdrive n8 pro over usb host:\n"
           "  n            USB link state / enumerated device\n"
           "  a            open the link: wake, identify, sys info\n"
           "  o <addr> <n> mem_rd + hexdump (addr accepts 0x...)\n"
           "  v [bytes]    PSRAM round-trip memtest (default 4096)\n"
           "  y [bytes]    mem_wr/mem_rd throughput bench (default 256K)\n"
           "  E            staged cart payload: manifest + crc (E, not e)\n"
           "  W            write it to the cart's SD and boot it (needs the menu)\n"
           "game disc (rom + music tracks, autoplay boots it):\n"
           "  L [v]        the catalog; L v also reads every item's crc\n"
           "  J [s|x|d]    stage the ROM item, push it to the cart and boot (s: stage\n"
           "               only; x: cart-ROM mode, the ROM on the cart is the game and\n"
           "               the disc's is never pushed; d: back to disc mode)\n"
           "  G [0|1]      arm the game's music mailbox against the open disc\n"
           "  S            track reader statistics and buffer depth\n"
           "  O [0|1]      autoplay on insertion, and the disc's ROM on the menu (now %s)\n"
           "  Y            spin the drive up after a failed acquisition\n"
           "  (the panel button on GP33 toggles the tray, between commands)\n"
           "audio cd player (the NCDP ROM on the cart drives it through the mailbox):\n"
           "  C            state, track, the drive's cd-da capability\n"
           "  C t          the disc's table of contents\n"
           "  C p [n]      play track n (or resume); C h pause/resume; C s stop\n"
           "  C n | v      next / previous track;  C e | l  eject / load\n"
           "  G p          arm the player by hand (it arms itself on the ROM)\n"
           "audio out (pcm5102 i2s dac):\n"
           "  M            toggle the jingle\n"
           "  B [track]    play track n off the disc as background music, its loop\n"
           "               range honored; bare B stops (the game's mailbox\n"
           "               overrides while G is armed)\n"
           "  V [-dB|k]    output level, 0 to -%d dBFS (now %d); V k = panel pot\n"
           "  T [hz] [l|r] toggle a full-scale sine for level calibration\n"
           "on-board psram:\n"
           "  P [bytes]    size, memtest, throughput, cache checks (default 1M)\n"
           "wiring diagnostics (no live drive needed):\n"
           "  g            snapshot every line: GPIO, ATA pin, level\n"
           "  f            float scan - which lines nothing is driving\n"
           "  w            short scan - which lines are tied together\n"
           "  p <gpio>     toggle one GPIO at 2Hz for 10s, to trace it\n"
           "  m            live monitor, print on any change\n\n",
           ata_get_pio_mode(), (unsigned long)ata_get_cycle_ns(),
           autoplay_on ? "on" : "off", AUDIO_ATTEN_MAX, audio_get_atten_db());
}

static void show_status(void)
{
    uint8_t s = ata_altstatus();
    printf("  status %02x [%s%s%s%s%s]\n", s,
           (s & ATA_ST_BSY)  ? "BSY "  : "",
           (s & ATA_ST_DRDY) ? "DRDY " : "",
           (s & ATA_ST_DSC)  ? "DSC "  : "",
           (s & ATA_ST_DRQ)  ? "DRQ "  : "",
           (s & ATA_ST_ERR)  ? "ERR "  : "");
}

static void report(const char *what, int rc)
{
    if (rc == ATAPI_OK) { printf("  %s: ok\n", what); return; }
    printf("  %s: %s", what, atapi_strerror(rc));
    if (rc == ATAPI_ECHECK)
        printf(" - sense %x/%02x/%02x (%s)", atapi_sense_key, atapi_sense_asc,
               atapi_sense_ascq, atapi_sense_text(atapi_sense_key));
    printf("\n");
}

// A drive can hold itself unready for many seconds after a reset; the standard
// allows up to 31. Report the ATAPI signature once it settles.
static void do_reset_wait(void)
{
    uint8_t mid = 0, high = 0;
    if (!ata_wait_signature(31000)) {
        ata_read_signature(&mid, &high);
        printf("  no ATAPI signature after 31s (got %02x/%02x, status %02x)\n",
               mid, high, ata_altstatus());
        return;
    }
    ata_read_signature(&mid, &high);
    printf("  ready, signature %02x/%02x (ATAPI), status %02x\n",
           mid, high, ata_altstatus());
}

static void do_identify(void)
{
    if (!ata_identify_packet(identify)) {
        printf("  IDENTIFY PACKET DEVICE failed\n");
        show_status();
        return;
    }
    char s[64];
    ata_id_string(identify, 27, 20, s, sizeof(s)); printf("  model:    %s\n", s);
    ata_id_string(identify, 23,  4, s, sizeof(s)); printf("  firmware: %s\n", s);
    ata_id_string(identify, 10, 10, s, sizeof(s)); printf("  serial:   %s\n", s);
    printf("  config:   %04x %s\n", identify[0],
           ((identify[0] >> 14) == 2) ? "(ATAPI packet device)" : "(not ATAPI?)");
    printf("  pio:      up to mode %u, iordy %s",
           (unsigned)(identify[51] >> 8),
           (identify[49] & (1u << 11)) ? "yes" : "no");
    if (identify[53] & 2u) printf(", advanced modes %02x", identify[64] & 3u);
    printf("\n");
}

// Word 51's high byte is the highest PIO mode the drive claims. Modes 3 and 4
// live in word 64 and need IORDY, which this host does not honor.
static void do_timing(const char *arg)
{
    if (*arg) {
        uint m = (uint)strtoul(arg, NULL, 10);
        if (m > ATA_PIO_MODE_MAX) {
            printf("  mode %u needs IORDY flow control; 0..%u only\n",
                   m, ATA_PIO_MODE_MAX);
            return;
        }
        uint claimed = identify[51] >> 8;
        if (identify[0] && m > claimed)
            printf("  warning: drive claims only PIO mode %u\n", claimed);
        if (m > 0)
            printf("  warning: mode %u entitles the drive to assert IORDY,\n"
                   "           which this host ignores - re-check d <lba>\n", m);
        if (!ata_set_xfer_mode(m))
            printf("  warning: SET FEATURES rejected, host retimed anyway\n");
        ata_set_pio_mode(m);
    }
    ata_timing_report();
}

// Proof that the bus no longer owns the CPU: count how many times the USB
// host stack gets pumped while a sector is in flight.
static void do_async(uint32_t lba)
{
    uint8_t cdb[ATAPI_CDB_LEN] = {
        ATAPI_READ10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, 0, 1, 0, 0, 0
    };

    if (!ata_wait_not_bsy(5000)) { printf("  drive busy\n"); return; }
    ata_select_device(0);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_FEATURES,  0);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_BCOUNT_LO, SECTOR_BYTES & 0xFFu);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_BCOUNT_HI, (SECTOR_BYTES >> 8) & 0xFFu);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_COMMAND,   ATA_CMD_PACKET);
    if (!ata_wait_drq(5000)) { printf("  no DRQ for the CDB\n"); return; }

    uint16_t words[ATAPI_CDB_LEN / 2];
    memcpy(words, cdb, ATAPI_CDB_LEN);
    ata_write_data_burst(words, ATAPI_CDB_LEN / 2);

    if (!ata_wait_drq(30000)) { printf("  no data DRQ\n"); return; }
    if (!ata_read_begin((uint16_t *)sector, SECTOR_BYTES / 2)) {
        printf("  async path declined (alignment or a read in flight)\n");
        return;
    }

    uint32_t pumps = 0;
    while (!ata_read_poll()) { usb_link_pump(); pumps++; }
    ata_read_end();

    ata_wait_not_bsy(5000);
    printf("  LBA %lu read, %lu USB pump(s) during the transfer\n",
           (unsigned long)lba, (unsigned long)pumps);
    hexdump(sector, 64);
}

static void do_capacity(void)
{
    uint32_t last = 0, bsz = 0;
    int rc = atapi_read_capacity(&last, &bsz);
    if (rc != ATAPI_OK) { report("READ CAPACITY", rc); return; }
    printf("  last LBA %lu, block %lu bytes, %lu MiB\n",
           (unsigned long)last, (unsigned long)bsz,
           (unsigned long)(((uint64_t)(last + 1) * bsz) >> 20));
}

static void do_bulk(uint32_t lba, uint32_t nsectors)
{
    if (nsectors == 0) { printf("  usage: b <lba> <sectors>\n"); return; }

    ata_stat_words = 0;
    ata_stat_bus_us = 0;
    absolute_time_t t0 = get_absolute_time();
    uint32_t done = 0;
    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        if (chunk > BULK_SECTORS) chunk = BULK_SECTORS;
        size_t got = 0;
        int rc = atapi_read10(lba + done, (uint16_t)chunk, bulk,
                              chunk * SECTOR_BYTES, &got);
        if (rc != ATAPI_OK) {
            printf("  stopped at LBA %lu after %lu sectors\n",
                   (unsigned long)(lba + done), (unsigned long)done);
            report("READ(10)", rc);
            return;
        }
        done += chunk;
    }
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    if (us <= 0) us = 1;

    uint64_t bytes = (uint64_t)done * SECTOR_BYTES;
    printf("  %lu sectors (%lu KiB) in %lu ms = %lu KB/s wall\n",
           (unsigned long)done, (unsigned long)(bytes >> 10),
           (unsigned long)(us / 1000), (unsigned long)(bytes * 1000 / (uint64_t)us));

    // Splitting bus time out of wall time says whether the next win belongs in
    // the bus driver, in per-DRQ overhead, or in read-ahead.
    uint64_t bus_us = ata_stat_bus_us ? ata_stat_bus_us : 1;
    printf("    bus %lu words in %lu ms = %lu KB/s, %lu%% bus-bound\n",
           (unsigned long)ata_stat_words, (unsigned long)(bus_us / 1000),
           (unsigned long)(ata_stat_words * 2 * 1000 / bus_us),
           (unsigned long)(bus_us * 100 / (uint64_t)us));
}

static void do_raw(const char *arg)
{
    uint8_t cdb[ATAPI_CDB_LEN];
    char tmp[96];
    int n = 0;

    memset(cdb, 0, sizeof(cdb));
    strncpy(tmp, arg, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
    for (char *tok = strtok(tmp, " "); tok && n < ATAPI_CDB_LEN; tok = strtok(NULL, " "))
        cdb[n++] = (uint8_t)strtoul(tok, NULL, 16);
    if (n == 0) { printf("  usage: r <hex bytes>\n"); return; }

    size_t got = 0;
    int rc = atapi_packet(cdb, sector, sizeof(sector), &got);
    printf("  CDB %02x, %u byte(s) back\n", cdb[0], (unsigned)got);
    if (got) hexdump(sector, (int)(got > 64 ? 64 : got));
    if (rc != ATAPI_OK) report("packet", rc);
}

// -- cart link -------------------------------------------------------------

static void n8_report(const char *what, int rc)
{
    if (rc == EDN8_OK) { printf("  %s: ok\n", what); return; }
    printf("  %s: %s", what, edn8_strerror(rc));
    if (rc == EDN8_ESTATUS)  printf(" - status 0x%02X", edn8_last_status);
    if (rc == EDN8_ETIMEOUT) printf(" - moved %u byte(s)", (unsigned)edn8_last_short);
    if (rc == EDN8_EWEDGED)  printf(" - %lu byte(s) owed, POWER-CYCLE the cart",
                                    (unsigned long)edn8_bytes_owed);
    printf("\n");
}

static const char *speed_name(uint8_t s)
{
    switch (s) {
    case 0:  return "full";      // tusb_speed_t: 0 full, 1 low, 2 high
    case 1:  return "low";
    case 2:  return "high";
    default: return "?";
    }
}

static void do_usb_state(void)
{
    const usb_link_state_t *s = usb_link_state();
    uint32_t sie = usb_link_sie_status();
    uint32_t bus_speed = (sie & USB_SIE_STATUS_SPEED_BITS) >> USB_SIE_STATUS_SPEED_LSB;

    // SPEED is the durable "something is on the bus" signal; CONNECTED is a
    // latch the host stack clears once it has handled the connect.
    printf("  sie:      0x%08lx [%s%s%s]\n", (unsigned long)sie,
           bus_speed == 2 ? "FS " : bus_speed == 1 ? "LS " : "no device ",
           (sie & USB_SIE_STATUS_VBUS_DETECTED_BITS) ? "VBUS " : "",
           (sie & USB_SIE_STATUS_CONNECTED_BITS) ? "CONN-latch" : "");
    printf("  events:   %lu attach, %lu remove\n",
           (unsigned long)s->attach_count, (unsigned long)s->remove_count);
    if (s->attach_count == 0) { printf("  device:   none\n"); return; }

    printf("  device:   addr %u, %04X:%04X, %s-speed\n",
           s->daddr, s->vid, s->pid, speed_name(s->speed));
    printf("  desc:     bcdUSB %04X class %02X/%02X/%02X mps0 %u cfgs %u bcdDevice %04X\n",
           s->bcd_usb, s->dev_class, s->dev_subclass, s->dev_protocol,
           s->ep0_size, s->num_cfg, s->bcd_device);
    printf("  bulk mps: in %u, out %u\n", s->bulk_in_mps, s->bulk_out_mps);
    printf("  mfr:      %s\n", s->mfr);
    printf("  product:  %s\n", s->product);
    printf("  serial:   %s\n", s->serial);
    if (s->mounted) printf("  cdc:      idx %u, itf %u, mounted\n", s->cdc_idx, s->cdc_itf);
    else            printf("  cdc:      not mounted\n");
}

static void do_n8_open(void)
{
    int rc = edn8_open(2000);
    if (rc != EDN8_OK) { n8_report("open", rc); return; }

    edn8_ident_t id;
    edn8_ident(&id);
    printf("  identify: key %02X, protocol %02X, device %02X (N8 PRO)\n",
           EDN8_STATUS_KEY, id.protocol, id.device);

    uint8_t st = 0;
    rc = edn8_status(&st);
    if (rc != EDN8_OK) { n8_report("status", rc); return; }
    printf("  status:   %02X\n", st);

    static edn8_sysinfo_t info;
    rc = edn8_sys_info(&info);
    if (rc != EDN8_OK) { n8_report("sys info", rc); return; }
    printf("  serial:   %08lX.%08lX\n",
           (unsigned long)info.serial_hi, (unsigned long)info.serial_lo);
    printf("  form:     %s\n", info.cart_form ? "FAMICOM" : "NES");
    printf("  os/hw/boot: %04X / %04X / %04X\n", info.sw_ver, info.hw_ver, info.boot_ver);
    printf("  flash:    %lu MiB\n", (unsigned long)(info.flash_size >> 20));
    printf("  game/boot ctr: %lu / %lu\n",
           (unsigned long)info.game_ctr, (unsigned long)info.boot_ctr);
}

// Nonzero pending RX means the stream is already out of step; say so rather
// than draining it, which would hide the bug.
static bool n8_ready(void)
{
    if (!edn8_is_open()) { printf("  not open - press a first\n"); return false; }
    size_t pend = edn8_rx_pending();
    if (pend) printf("  warning: %u stale byte(s) in the rx buffer\n", (unsigned)pend);
    return true;
}

static void do_n8_read(const char *arg)
{
    char *end = NULL;
    uint32_t addr = (uint32_t)strtoul(arg, &end, 0);
    uint32_t n    = (uint32_t)strtoul(end ? end : "", NULL, 0);

    if (n == 0) { printf("  usage: o <addr> <n>\n"); return; }
    if (n > sizeof(sector)) n = sizeof(sector);
    if (!n8_ready()) return;

    int rc = edn8_mem_rd(addr, sector, n);
    if (rc != EDN8_OK) { n8_report("mem_rd", rc); return; }
    printf("  read %lu bytes from 0x%07lX:\n", (unsigned long)n, (unsigned long)addr);
    hexdump(sector, (int)(n > 256 ? 256 : n));
}

static void do_n8_memtest(const char *arg)
{
    size_t size = *arg ? (size_t)strtoul(arg, NULL, 0) : 4096u;
    static edn8_rt_result_t r[2];
    static const char *name[2] = { "CHR", "PRG" };

    if (size == 0) { printf("  usage: v [bytes]\n"); return; }
    if (!n8_ready()) return;

    printf("  memtest:  %u B round-trip (mem_wr -> mem_rd)\n", (unsigned)size);
    edn8_memtest(size, r);
    for (int i = 0; i < 2; i++) {
        if (r[i].ok) {
            printf("  %-3s round-trip PASS  (%u B @ 0x%07lX)\n",
                   name[i], (unsigned)r[i].size, (unsigned long)r[i].addr);
        } else if (r[i].rc == EDN8_EVERIFY) {
            printf("  %-3s round-trip FAIL  (%u B @ 0x%07lX, first diff @ %u)\n",
                   name[i], (unsigned)r[i].size, (unsigned long)r[i].addr,
                   (unsigned)r[i].first_diff);
        } else {
            n8_report(name[i], r[i].rc);
        }
    }
}

static void rate_line(const char *what, size_t bytes, uint32_t us, const char *tail)
{
    if (us == 0) us = 1;
    printf("  %-9s %lu KiB in %lu ms = %lu KB/s%s\n", what,
           (unsigned long)(bytes >> 10), (unsigned long)(us / 1000),
           (unsigned long)((uint64_t)bytes * 1000u / us), tail);
}

static void do_n8_bench(const char *arg)
{
    size_t size = *arg ? (size_t)strtoul(arg, NULL, 0) : 262144u;
    static edn8_bench_result_t b;

    if (size == 0) { printf("  usage: y [bytes]\n"); return; }
    if (!n8_ready()) return;

    int rc = edn8_bench(EDN8_ADDR_PRG + 0x20000u, size, true, &b);
    rate_line("wr:", b.size, b.wr_us, "");
    if (b.wr_barrier_us) rate_line("barrier:", b.size, b.wr_barrier_us, "");
    if (b.rd_us) rate_line("rd:", b.size, b.rd_us, b.match ? " [ok]" : " [MISMATCH]");
    if (!b.match && b.first_diff != (size_t)-1)
        printf("  first diff @ %u\n", (unsigned)b.first_diff);
    if (rc != EDN8_OK) n8_report("bench", rc);
}

static void do_push_info(void)
{
    int rc = n8push_open(true);
    if (rc != CD_OK) {
        printf("  staged @ %08X: %s\n", N8PUSH_BASE, cd_strerror(rc));
        return;
    }
    const n8push_hdr_t *h = n8push_hdr();
    printf("  staged @ %08X: %lu file(s), %lu KiB, crc %08lX [ok]\n",
           N8PUSH_BASE, (unsigned long)h->nfiles,
           (unsigned long)(h->payload_bytes >> 10), (unsigned long)h->crc32);
    for (uint32_t i = 0; i < h->nfiles; i++)
        printf("    %-32s %lu B\n", h->file[i].path, (unsigned long)h->file[i].len);
    if (h->boot[0]) printf("  boot:     %s\n", h->boot);
    else            printf("  boot:     none - push only\n");
}

static void do_push_run(void)
{
    if (!n8_ready()) return;

    int rc = n8push_open(true);
    if (rc == CD_OK) rc = n8push_run();
    if (rc != CD_OK) {
        printf("  push: failed - %s", cd_strerror(rc));
        if (rc == EDN8_ESTATUS)  printf(" - status 0x%02X", edn8_last_status);
        if (rc == EDN8_ETIMEOUT) printf(" - moved %u byte(s)", (unsigned)edn8_last_short);
        if (rc == EDN8_EWEDGED)  printf(" - %lu byte(s) owed, POWER-CYCLE the cart",
                                        (unsigned long)edn8_bytes_owed);
        printf("\n");
        if (rc == CD_EMENU)
            printf("  press the console's reset button, let the menu come up, "
                   "then retry\n");
        return;
    }
    printf("  push: ok\n");
}

// -- the track reader ------------------------------------------------------

static void do_disc_stats(void)
{
    const disc_stat_t *t = &disc_stat;
    const disc_job_t *job = disctask_job();
    printf("  %s: %s, %lu blocks, %lu lap(s), next seq %lu\n",
           job ? job->name : "producer", t->running ? "running" : "idle",
           (unsigned long)t->blocks, (unsigned long)t->laps,
           (unsigned long)(job == &cdda_job ? cdda_next_seq() : track_next_seq()));
    printf("  reads:    %lu chunks, %lu retries, worst %lu ms, settle %lu\n",
           (unsigned long)t->chunks, (unsigned long)t->retries,
           (unsigned long)t->worst_read_ms, (unsigned long)disc_settle_tries);
    if (t->last_rc)
        printf("  last err: rc %ld, sense %lu/%02lX/%02lX\n", (long)t->last_rc,
               (unsigned long)(t->last_sense >> 16),
               (unsigned long)((t->last_sense >> 8) & 0xFF),
               (unsigned long)(t->last_sense & 0xFF));
    if (disc_fail_tries) {
        printf("  failed:   lba %lu after %lu read(s), attempts:",
               (unsigned long)disc_fail_lba, (unsigned long)disc_fail_tries);
        for (uint32_t i = 0; i < DISC_READ_TRIES; i++) {
            uint32_t s = disc_fail_sense[i];
            if (s == 0xFFFFFFFFu) printf(" no-sense");
            else printf(" %lu/%02lX/%02lX", (unsigned long)(s >> 16),
                        (unsigned long)((s >> 8) & 0xFF), (unsigned long)(s & 0xFF));
        }
        printf("\n");
    }
    bgm_report();
    player_report();
}

// -- the tray, and autoplay --------------------------------------------------
//
// Polled from the console's idle loop and nowhere else, so core1 is parked when
// TEST UNIT READY goes out. One command a second, and only once a drive has
// answered IDENTIFY - against a dead or absent bus every packet costs its full
// 5 s timeout. The poll identifies whatever goes in; autoplay only decides
// whether a game disc boots.

#define AUTOPLAY_POLL_MS 1000u
#define AUTOPLAY_IDLE_MS 10000u      // a shut tray with no medium: stop poking it
#define AUTOPLAY_BOOT_MS 3000u       // after the prompt: room to type J x first

static void game_disc_insert(void);
static void disc_gone(void);

// A track streams between commands, and then core1 owns the drive: nothing on
// core0 may issue an ATA command until it stops.
static bool bus_free(void)
{
    if (!disctask_busy()) return true;
    printf("  core1 owns the drive (%s streaming) - B or C s stops it\n",
           disctask_job()->name);
    return false;
}

#define GAME_OPEN_POLLS 10u          // catalog reads to retry before an insertion is dropped

static bool media_present;           // the tray, as of the last poll
static bool autoplay_pending;        // an insertion still owed a boot
static bool autoplay_waiting;        // the "no cart yet" line is already out
static bool acquire_said;            // the failed-acquisition line is already out
static uint32_t open_fails;          // catalog reads that failed on this insertion
static absolute_time_t autoplay_due;

// TEST UNIT READY is the whole detector: ok is a mounted, spun-up disc and
// 2/3A/xx an empty or open tray. Everything else - 6/28/00 right after a
// change, 2/04/xx during spin-up - is the drive settling rather than an edge,
// so the next poll decides.
static bool media_shut_empty;        // 3A with the tray shut: acquisition failed

static int media_state(void)
{
    int rc = atapi_test_unit_ready();
    if (rc == ATAPI_OK) { media_shut_empty = false; return 1; }
    if (rc == ATAPI_ECHECK && atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A) {
        media_shut_empty = (atapi_sense_ascq != 0x02);
        return 0;
    }
    return -1;
}

// Acquisition is what a marginal rail breaks, never reading: the spindle's
// starting current costs volts across the switch, the two molex hops and the
// board, and the focus servo never locks -- while steady reads at a fraction of
// that current run 0-retry. So shed our own draw on the shared 5 V first, then
// escalate. TEST UNIT READY alone will poll 2/3A/01 forever: the drive has
// given up and only a START UNIT or a bus reset makes it try again.
#define ACQUIRE_ROUNDS  4u
#define ACQUIRE_SETTLE_MS 1500u

static bool disc_acquire(void)
{
    bool played = audio_is_playing();
    if (played) { bgm_stop(); audio_stop(); }
    disc_speed_reset();                  // a bus reset returns the drive to its default

    bool ok = false;
    for (uint32_t r = 1; r <= ACQUIRE_ROUNDS && !ok; r++) {
        printf("  acquire: attempt %lu/%u, spin up\n", (unsigned long)r, ACQUIRE_ROUNDS);
        atapi_start_stop(ATAPI_SS_START);
        sleep_ms(ACQUIRE_SETTLE_MS);
        if ((ok = (media_state() == 1))) break;

        printf("  acquire: attempt %lu/%u, bus reset\n", (unsigned long)r, ACQUIRE_ROUNDS);
        ata_hard_reset();
        if (ata_wait_signature(5000)) ata_identify_packet(identify);
        atapi_wait_ready(8000);
        sleep_ms(ACQUIRE_SETTLE_MS);
        ok = (media_state() == 1);
    }
    printf("  acquire: %s\n", ok ? "medium acquired" : "gave up");
    if (played && !ok) audio_start(jingle_fill);
    return ok;
}

// True when it printed, so the caller can redraw the prompt.
static bool media_poll(void)
{
    if (!identify[0]) return false;
    if (disctask_busy()) return false;           // a producer owns the drive
    if (!time_reached(autoplay_due)) return false;
    autoplay_due = make_timeout_time_ms(AUTOPLAY_POLL_MS);

    int st = media_state();
    if (st < 0) return false;
    player_media(st, media_shut_empty);
    bool was = media_present;
    media_present = (st == 1);
    if (!media_present) {
        autoplay_pending = autoplay_waiting = false;
        if (was) disc_gone();
        // A shut tray reporting no medium is a failed acquisition, not an empty
        // drive, and the two are otherwise identical from here: both go quiet.
        if (!media_shut_empty) { acquire_said = false; return false; }
        // Never spin the drive from a poll. A drive that cannot acquire is
        // mechanically busy already, and a 1 Hz retry grinds it indefinitely;
        // recovery is a command the user issues, not something a loop decides.
        autoplay_due = make_timeout_time_ms(AUTOPLAY_IDLE_MS);
        if (acquire_said) return false;
        acquire_said = true;
        printf("\nautoplay: tray shut but no medium acquired (sense 2/3A/%02X)"
               " - press Y to spin it up\n", atapi_sense_ascq);
        return true;
    }
    acquire_said = false;
    // A tray edge boots, and a disc found in the drive at power-up is one: the
    // cart-state poll is what makes that safe, since nothing is pushed until
    // the cart shows its menu.
    if (!was) { autoplay_pending = true; open_fails = 0; }
    if (!autoplay_pending) return false;

    // The kind of disc first, off its TOC: a READ(10) of an audio track fails
    // 5/64 and would be retried for a minute before the catalog gave up.
    if (!cdda_is_open() && !catalog_is_open()) {
        int rc = cdda_open();
        if (rc == CD_OK) {
            autoplay_pending = autoplay_waiting = false;
            const cdda_toc_t *t = cdda_toc();
            uint8_t m, s;
            cdda_msf(t->leadout - t->start[t->first], &m, &s);
            printf("\nmedia: audio cd, %u track(s), %u:%02u%s\n", t->last, m, s,
                   player_armed() ? "" : " - start the CD player ROM to play it");
            player_disc_in();
            return true;
        }
        if (rc == CD_EDISCIO && ++open_fails < GAME_OPEN_POLLS) {
            if (open_fails > 1) return false;
            printf("\nmedia: toc read failed (sense %u/%02X/%02X) - retrying\n",
                   atapi_sense_key, atapi_sense_asc, atapi_sense_ascq);
            return true;
        }
        if (rc != CD_ENOAUDIO && rc != CD_ESHAPE) {
            autoplay_pending = autoplay_waiting = false;
            printf("\nmedia: %s\n", cd_strerror(rc));
            return true;
        }
        player_disc_other();
    }
    if (!autoplay_on) {
        autoplay_pending = autoplay_waiting = false;
        printf("\nmedia: data disc in, autoplay off\n");
        return true;
    }

    // The cart is the one thing worth waiting for: the NES may still be coming
    // up when the disc goes in, so the insertion stays pending. edn8_open() is
    // only tried once a device has enumerated, or it burns 2 s of every poll.
    if (!edn8_is_open() &&
        (!usb_link_state()->mounted || edn8_open(2000) != EDN8_OK)) {
        if (autoplay_waiting) return false;
        autoplay_waiting = true;
        printf("\nautoplay: disc in, waiting for the cart link\n");
        return true;
    }

    // The catalog opens once; cart_poll() takes it from there. A read that
    // fails is retried on the next polls - the first read after SET CD SPEED
    // lands in the drive's recalibration window.
    int rc = catalog_is_open() ? CD_OK : catalog_open();
    if (rc == CD_OK) {
        autoplay_pending = autoplay_waiting = false;
        game_disc_insert();
        return true;
    }
    if (rc == CD_EDISCIO && ++open_fails < GAME_OPEN_POLLS) {
        if (open_fails > 1) return false;
        printf("\nautoplay: catalog read failed (sense %u/%02X/%02X) - retrying\n",
               atapi_sense_key, atapi_sense_asc, atapi_sense_ascq);
        return true;
    }
    autoplay_pending = autoplay_waiting = false;
    if (rc == CD_ENOGAME) printf("\nautoplay: disc inserted, no game catalog on it\n");
    else                  printf("\nautoplay: %s\n", cd_strerror(rc));
    return true;
}

static void do_autoplay(const char *arg)
{
    if (*arg == '0' || *arg == '1') {
        bool on = (*arg == '1');
        // The tray is tracked either way; enabling only forgets an insertion
        // still owed, so a disc already in does not boot until the next one.
        if (on && !autoplay_on) autoplay_pending = autoplay_waiting = false;
        autoplay_on = on;
    }
    printf("  autoplay: %s\n", autoplay_on ? "on" : "off");
}

// -- the game's mailbox ----------------------------------------------------
//
// $1FF8 is the game's request, $1FF9 the desired music track. Polled from the
// same idle-loop slot as autoplay, so a poll can never land while core0 is
// inside a cart operation.

#define GAME_POLL_MS 250u
// A freshly booted ROM has to warm the PPU up and upload 8 KiB of tiles before
// it owns the mailbox, so the first poll waits out that window.
#define GAME_BOOT_POLL_MS 750u
#define GAME_FAIL_WARN 8u            // 2 s of dead reads before it is worth saying
#define MAILBOX_REQ_MAX 8u           // highest mailbox value read as a request
#define BGM_TRACK_MAX 32u            // highest music byte read as a track request
#define TRACK_START_MS 3000u         // first chunk of a track: a seek plus a read

static bool game_on;
static bool game_seen_zero;          // the game holds the screen; a 1 is a trigger
static bool game_waiting;            // the "no cart link" line is already out
static uint32_t game_read_fails;     // consecutive reads that told us nothing
static absolute_time_t game_due;
static uint32_t bgm_refused;         // track that failed to open: wait for a change
static uint32_t bgm_done;            // non-loop track that played out: same
static uint32_t track_start;         // where the producer was armed

// SRC_NONE is "armed, but the disc it was armed against is gone".
typedef enum { SRC_NONE, SRC_DISC } game_src_t;
static game_src_t game_src;

static uint32_t booted_crc;          // the ROM item pushed this cart session

// The cart's own state, polled once a second while a game disc is open.
#define CART_POLL_MS 1000u
#define CART_OPEN_RETRY_MS 5000u
static absolute_time_t cart_due;
static int  cart_st = -1;            // last edn8_status(), -1 unknown
static bool cart_mounted;            // the USB link, as of the last cart poll
static bool cart_boot_armed;         // the next menu appearance boots the ROM

// The cart's own ROM is the game and the disc's item is never pushed, so a ROM
// under development plays the disc's tracks. J x selects it, J d or a bare J
// returns to disc mode.
static bool rom_external;

// Survives a warm reset (every flash script), not a power-on, so a firmware
// reflash mid-game still knows the running ROM is the disc's and re-arms it.
#define BOOT_WARM_MAGIC 0xC0DE5A18u
__attribute__((section(".uninitialized_data.boot")))
static struct { uint32_t magic, crc; } boot_keep;

static void set_booted(uint32_t crc)
{
    booted_crc = crc;
    boot_keep.crc = crc;
}

// $1FFA is the host's answer to a $1FF8 request. This build serves no requests,
// so the only answer it ever gives is MAILBOX_FAIL; the end-of-session 0 still
// matters, since a stale byte would satisfy the next request.
static void status_wr(uint8_t v)
{
    if (!edn8_is_open()) return;
    int rc = mailbox_status_wr(v);
    if (rc != EDN8_OK) printf("  status byte: %s\n", edn8_strerror(rc));
}

static uint32_t track_consumer_seq(void)
{
    return bgm_active() ? bgm_cursor() : track_start;
}

// A game-disc read must not open the catalog over an audio cd: a READ(10) on
// an audio track spends the whole retry budget.
static bool audio_disc_in(void)
{
    if (!cdda_is_open()) return false;
    printf("  an audio cd is in - C plays it\n");
    return true;
}

// Open the item, start core1 at the resume point and wait for the first chunk
// of blocks before handing the DAC to bgm.c.
static int track_play(uint32_t id)
{
    if (!catalog_is_open()) return CD_ENOGAME;

    const cat_item_t *it = catalog_find(ITEM_TRACK, id);
    if (!it) return CD_ENOITEM;
    if (disctask_busy()) disctask_stop(35000);   // core1 has one job
    bgm_stop();                                  // before the slots are re-sized

    int rc = track_open(it->lba, it->sectors);
    if (rc != CD_OK) return rc;
    uint32_t start = bgm_resume_seq(id);
    track_start = start;
    disc_consumer = track_consumer_seq;
    if ((rc = disctask_track(start)) != CD_OK) return rc;

    absolute_time_t give = make_timeout_time_ms(TRACK_START_MS);
    while (disc_stat.blocks == 0 && disctask_busy() && !time_reached(give)) {
        usb_link_pump();
        sleep_ms(2);
    }
    if (disc_stat.blocks == 0) {
        int prc = disctask_stop(35000);
        return prc != CD_OK ? prc : CD_EDISCIO;
    }
    return bgm_play_disc(id, start);
}

// True when it printed, so the caller can redraw the prompt.
static bool game_poll(void)
{
    if (!game_on || player_armed()) return false;   // the player owns the page
    if (!time_reached(game_due)) return false;
    game_due = make_timeout_time_ms(GAME_POLL_MS);

    if (!edn8_is_open() &&
        (!usb_link_state()->mounted || edn8_open(2000) != EDN8_OK)) {
        if (game_waiting) return false;
        game_waiting = true;
        printf("\ngame: waiting for the cart link\n");
        return true;
    }
    game_waiting = false;

    // SRC_NONE is the disc having gone: nothing is left to serve, and cart_st is
    // frozen at whatever the last poll saw, since cart_poll stops with the
    // catalog.
    if (game_src == SRC_NONE) return false;

    // Off the menu the CHR at the mailbox is whatever was left there, and a
    // stale byte would be read as a request from a game that is not running.
    if (cart_st != (int)EDN8_ST_GAME) return false;

    // A disagreeing or failed read says nothing and the next tick asks again,
    // but a run of them is a link that is up and useless: the game's music
    // would never start and nothing would say why.
    uint8_t mb[2];
    int rc = mailbox_rd(mb);
    if (rc != CD_OK) {
        if (++game_read_fails != GAME_FAIL_WARN) return false;
        printf("\ngame: mailbox unreadable - %s\n", cd_strerror(rc));
        return true;
    }
    if (game_read_fails >= GAME_FAIL_WARN) {
        game_read_fails = 0;
        printf("\ngame: mailbox readable again\n");
        return true;
    }
    game_read_fails = 0;

    // A request is refused outright. Answering is what matters: a game left
    // waiting on $1FFA spends its whole RDY_TIMEOUT, 600 fields, on a black
    // screen before it gives up, where $FF puts it back in its room within a
    // field or two.
    if (mb[0] != 0 && mb[0] <= MAILBOX_REQ_MAX && game_seen_zero) {
        game_seen_zero = false;
        printf("\ngame: request %u - nothing in this build serves it\n", mb[0]);
        status_wr(MAILBOX_FAIL);
        return true;
    }
    if (mb[0] == 0 || mb[0] > MAILBOX_REQ_MAX) game_seen_zero = true;

    // The music byte is level-triggered: it IS the desired state, so nothing
    // needs a handshake. Bit 7 holds the track in place while the game pauses.
    bool hold = (mb[1] & BGM_HOLD) != 0;
    uint32_t want = mb[1] & ~BGM_HOLD;
    if (want > BGM_TRACK_MAX) want = 0;
    if (want != bgm_refused) bgm_refused = 0;
    if (want != bgm_done) bgm_done = 0;
    if (bgm_ended()) {
        bgm_done = bgm_track();   // played out: a still-high byte must not loop it
        bgm_stop();
    }
    if (!want) {
        // An explicit off drops the cursor so the next request starts from the
        // top. A jingle keeps playing to its end: the engine's own is shorter.
        bool was = bgm_track() != 0;
        if (was) printf("\ngame: music off\n");
        bgm_release();
        return was;
    }
    if (want == bgm_refused || want == bgm_done) return false;
    if (want == bgm_track()) { bgm_hold(hold); return false; }

    printf("\ngame: music track %lu requested\n", (unsigned long)want);
    rc = track_play(want);
    if (rc != CD_OK) {
        printf("  bgm: %s\n", cd_strerror(rc));
        bgm_refused = want;
    } else {
        bgm_hold(hold);
    }
    return true;
}

// Arming a game already running demands a 0 before the first trigger, so a
// mailbox found high is not replayed; $1FFA starts clear for the same reason.
//
// `fresh` is a ROM this pass just booted: n8push seeds its mailbox to 0, so
// anything nonzero there is real and demanding a 0 first would discard it. It
// also has a tile upload to finish, hence the longer first poll.
static void game_arm(game_src_t src, bool fresh)
{
    game_src = src;
    game_seen_zero = fresh;
    game_waiting = false;
    game_read_fails = 0;
    bgm_refused = bgm_done = 0;
    game_due = make_timeout_time_ms(fresh ? GAME_BOOT_POLL_MS : GAME_POLL_MS);
    game_on = true;
    status_wr(0);
}

static void game_disarm(void)
{
    game_on = false;
    bgm_forget();                            // nothing would ever stop it otherwise
    if (disctask_busy()) disctask_stop(35000);
    status_wr(0);
}

static void game_report(void)
{
    if (!game_on) { printf("  game trigger: off\n"); return; }
    if (game_src == SRC_DISC && catalog_is_open())
        printf("  game trigger: armed, disc \"%s\"\n", catalog_hdr()->title);
    else
        printf("  game trigger: armed, no source (disc removed)\n");
}

static void do_game(const char *arg)
{
    if (*arg == '0') { player_disarm(); game_disarm(); game_report(); player_report(); return; }
    if (*arg == 'p') { player_arm(false); player_report(); return; }
    if (*arg == '1') {
        if (!catalog_is_open()) { printf("  no game disc open\n"); return; }
        bgm_forget();                    // a saved cursor belongs to the old source
        game_arm(SRC_DISC, false);
    }
    game_report();
}

// -- booting the disc's game -----------------------------------------------

// The disc is out: whatever was open on it is gone with it.
static void disc_gone(void)
{
    if (catalog_is_open()) printf("  game disc removed\n");
    if (cdda_is_open()) printf("  audio cd removed\n");
    catalog_close();
    cdda_close();
    player_disc_out();
    disc_speed_reset();
    if (game_src == SRC_DISC) game_src = SRC_NONE;
    media_present = false;
    cart_boot_armed = false;
}

// Stage the ROM item in the idle half of PSRAM and spend the menu on it in one
// pass: every file, the install, the boot.
static int game_boot(const cat_item_t *rom)
{
    if (!disctask_ready()) return CD_EPSRAM;
    bgm_stop();                              // a reboot voids the music anyway
    if (disctask_busy()) disctask_stop(35000);   // core0 stages off the drive

    uint8_t *dst = (uint8_t *)PSRAM_XIP_BASE;
    size_t len = 0;
    printf("  rom:      %lu sectors at lba %lu\n", (unsigned long)rom->sectors,
           (unsigned long)rom->lba);
    int rc = catalog_stage_rom(rom, dst, ROM_STAGE_BYTES, &len);
    if (rc != CD_OK) return rc;
    if ((rc = n8push_open_at(dst, len, true)) != CD_OK) return rc;
    return n8push_run();
}

// Boot the disc's ROM on a cart showing its menu, and arm the mailbox.
static bool game_launch(void)
{
    const cat_item_t *rom = catalog_find(ITEM_ROM, 0);
    if (!rom) return false;
    int rc = game_boot(rom);
    if (rc != CD_OK) {
        printf("game: load failed - %s\n", cd_strerror(rc));
        return false;
    }
    set_booted(rom->crc32);
    cart_st = (int)EDN8_ST_GAME;             // the cart said it started it
    printf("game: booted \"%s\"\n", catalog_hdr()->title);
    game_arm(SRC_DISC, true);
    return true;
}

// A cart on its menu takes the ROM at once, one already running it is
// re-armed, anything else boots it when the menu next appears (cart_poll).
// In cart-ROM mode nothing is pushed: a running ROM is the game, whatever it is.
static void game_disc_insert(void)
{
    const cat_hdr_t *gh = catalog_hdr();
    const cat_item_t *rom = catalog_find(ITEM_ROM, 0);
    printf("\nautoplay: game disc \"%s\", %lu items\n", gh->title,
           (unsigned long)gh->nitems);
    if (!rom) { printf("autoplay: no rom item on it\n"); return; }

    uint8_t st = 0xFF;
    if (edn8_status(&st) != EDN8_OK) st = 0xFF;
    cart_st = st;
    if (rom_external) {
        printf("autoplay: cart-ROM mode, the disc's ROM is not pushed (J d ends it)\n");
        if (st != EDN8_ST_GAME) return;
        printf("autoplay: a ROM is running - armed against the disc\n");
        game_arm(SRC_DISC, true);
        return;
    }
    if (st == EDN8_ST_GAME && booted_crc == rom->crc32) {
        printf("autoplay: this game is already running - re-armed\n");
        game_arm(SRC_DISC, false);
        return;
    }
    if (st != EDN8_ST_MENU) {
        printf("autoplay: loads \"%s\" once the cart shows its menu\n", gh->title);
        cart_boot_armed = true;
        return;
    }
    game_launch();
}

// A ROM that just started gets this long before its mailbox page is read for
// the CD player's magic: its tile upload blanks the page first.
static bool magic_pending;
static absolute_time_t magic_due;

// The cart's own state, once a second: the menu with a game disc's ROM not
// running boots it, which is what a power-on, a reset and an insertion have
// in common, and a ROM that starts carrying the CD player's magic arms the
// player. cart_st gates every mailbox read.
static bool cart_poll(void)
{
    if (!time_reached(cart_due)) return false;
    cart_due = make_timeout_time_ms(CART_POLL_MS);

    bool mounted = usb_link_state()->mounted;
    if (cart_mounted && !mounted) {
        // Console off: the cart comes back on its menu, a new session.
        set_booted(0);
        cart_st = -1;
        cart_boot_armed = true;
        cart_mounted = false;
        magic_pending = false;
        player_disarm();
        printf("\ncart: link down - the game reloads once the menu is back\n");
        return true;
    }
    cart_mounted = mounted;
    if (!mounted) return false;
    // A cart that enumerates but does not answer costs 2 s per attempt, so
    // not every second.
    static absolute_time_t open_retry;
    if (!edn8_is_open()) {
        if (!time_reached(open_retry)) return false;
        open_retry = make_timeout_time_ms(CART_OPEN_RETRY_MS);
        if (edn8_open(2000) != EDN8_OK) return false;
    }

    uint8_t st;
    if (edn8_status(&st) != EDN8_OK) { cart_st = -1; return false; }
    int prev = cart_st;
    cart_st = st;
    if (st == EDN8_ST_GAME && prev != (int)EDN8_ST_GAME && !player_armed()) {
        magic_pending = true;
        magic_due = make_timeout_time_ms(GAME_BOOT_POLL_MS);
    }
    if (st != EDN8_ST_GAME) {
        magic_pending = false;
        if (player_armed()) {
            printf("\ncart: the CD player ROM is no longer running - player off\n");
            player_disarm();
            return true;
        }
    } else if (magic_pending && time_reached(magic_due)) {
        magic_pending = false;
        if (player_magic_ok()) {
            printf("\ncart: the CD player ROM is running - player armed\n");
            player_arm(true);
            return true;
        }
    }
    if (!catalog_is_open()) return false;    // the rest is the disc's ROM
    if (st == EDN8_ST_GAME) {
        cart_boot_armed = true;
        // Cart-ROM mode: a ROM that just started is the game, fresh.
        if (!rom_external || prev == (int)EDN8_ST_GAME) return false;
        printf("\ncart: a ROM is running - armed against the disc\n");
        game_arm(SRC_DISC, true);
        return true;
    }
    // O 0 holds the menu: the cart can be used by hand with the disc in.
    if (st != EDN8_ST_MENU || !cart_boot_armed || rom_external || !autoplay_on)
        return false;
    cart_boot_armed = false;
    printf("\ncart: menu up - loading \"%s\"\n", catalog_hdr()->title);
    game_launch();
    return true;
}

// A track producer runs between commands, so its end is noticed here rather
// than by a caller; the panel button stops it first, since the tray needs the
// bus.
static bool task_poll(void)
{
    if (disctask_busy()) {
        if (!eject_pressed()) return false;
        printf("\n  button: stopping core1 to work the tray\n");
        bgm_stop();
        disctask_stop(35000);
        eject_toggle();
        return true;
    }
    int rc;
    if (!disctask_reap(&rc)) return false;
    const disc_job_t *job = disctask_job();
    if (rc == CD_EMEDIUM) {
        printf("\n%s: the disc was removed\n", job->name);
        bgm_stop();
        disc_gone();
        return true;
    }
    if (job == &cdda_job) return player_reap(rc);
    if (rc != CD_OK) {
        printf("\ntrack: %s\n", cd_strerror(rc));
        // Read it first: bgm_stop() releases the DAC, and bgm_track() is 0
        // once it has. Latching 0 here would retry the dead track forever.
        uint32_t was = bgm_track();
        bgm_stop();
        bgm_refused = was;
        return true;
    }
    return false;                            // played out: bgm_ended() reports it
}

// -- game disc console -----------------------------------------------------

static bool item_tick(uint32_t done, uint32_t total)
{
    static uint32_t last_pct = 0xFFFFFFFFu;
    uint32_t pct = (uint32_t)((uint64_t)done * 100u / total);
    if (pct != last_pct) { last_pct = pct; printf("\r    %lu%%  ", (unsigned long)pct); }
    return getchar_timeout_us(0) == PICO_ERROR_TIMEOUT;
}

static void do_catalog_info(const char *arg)
{
    if (audio_disc_in()) return;
    int rc = catalog_open();
    if (rc != CD_OK) { printf("  game disc: %s\n", cd_strerror(rc)); return; }
    const cat_hdr_t *h = catalog_hdr();
    printf("  game disc: \"%s\", v%lu, %lu items\n", h->title,
           (unsigned long)h->version, (unsigned long)h->nitems);
    printf("  %-6s %3s %8s %8s  %s\n", "type", "id", "lba", "sectors", "crc32");
    for (uint32_t i = 0; i < h->nitems; i++) {
        const cat_item_t *it = &h->item[i];
        printf("  %-6s %3lu %8lu %8lu  %08lX", catalog_type_name(it->type),
               (unsigned long)it->id, (unsigned long)it->lba,
               (unsigned long)it->sectors, (unsigned long)it->crc32);
        if (*arg == 'v') {
            rc = catalog_verify_item(it, item_tick);
            printf("\r  %-6s %3lu %8lu %8lu  %08lX  %s", catalog_type_name(it->type),
                   (unsigned long)it->id, (unsigned long)it->lba,
                   (unsigned long)it->sectors, (unsigned long)it->crc32,
                   rc == CD_OK ? "ok" : cd_strerror(rc));
            if (rc == CD_EDISCIO) { printf("\n"); return; }
        }
        printf("\n");
    }
}

static void do_game_load(const char *arg)
{
    if (*arg == 'x' || *arg == 'd') {
        rom_external = (*arg == 'x');
        printf("  rom: %s\n", rom_external
               ? "the cart's own ROM is the game; the disc's item is not pushed"
               : "the disc's ROM item boots whenever the cart shows its menu");
        return;
    }
    if (!catalog_is_open()) {
        if (audio_disc_in()) return;
        int rc = catalog_open();
        if (rc != CD_OK) { printf("  game disc: %s\n", cd_strerror(rc)); return; }
    }
    const cat_item_t *rom = catalog_find(ITEM_ROM, 0);
    rom_external = false;                    // an explicit load is disc mode
    if (*arg == 's') {
        uint8_t *dst = (uint8_t *)PSRAM_XIP_BASE;
        size_t len = 0;
        int rc = catalog_stage_rom(rom, dst, ROM_STAGE_BYTES, &len);
        if (rc == CD_OK) rc = n8push_open_at(dst, len, true);
        if (rc != CD_OK) { printf("  rom: %s\n", cd_strerror(rc)); return; }
        const n8push_hdr_t *h = n8push_hdr();
        printf("  staged in psram: %lu file(s), %lu KiB, crc %08lX [ok]\n",
               (unsigned long)h->nfiles, (unsigned long)(h->payload_bytes >> 10),
               (unsigned long)h->crc32);
        for (uint32_t i = 0; i < h->nfiles; i++)
            printf("    %-32s %lu B\n", h->file[i].path, (unsigned long)h->file[i].len);
        printf("  boot:     %s\n", h->boot[0] ? h->boot : "none");
        return;
    }
    if (!n8_ready()) return;
    if (!game_launch()) {
        printf("  press the console's reset button, let the menu come up, "
               "then retry\n");
        return;
    }
    game_report();
}

// Console side of the music player: B <n> plays track n off the open disc,
// bare B stops. While G is armed the game's mailbox overrides this on its
// next tick.
static void do_bgm(const char *arg)
{
    if (*arg) {
        uint32_t n = (uint32_t)strtoul(arg, NULL, 10);
        if (!n) { printf("  usage: B [track], tracks number from 1\n"); return; }
        // B is outside the bus gate, because bare B is how core1 is stopped.
        // So the stop has to come before core0's first read, not inside
        // track_play() after the catalog has already been read.
        if (disctask_busy()) disctask_stop(35000);
        if (!catalog_is_open() && media_present) {
            if (audio_disc_in()) return;
            int rc = catalog_open();
            if (rc != CD_OK) { printf("  game disc: %s\n", cd_strerror(rc)); return; }
        }
        int rc = track_play(n);
        if (rc != CD_OK) printf("  track %lu: %s\n", (unsigned long)n, cd_strerror(rc));
        return;
    }
    bgm_stop();
    if (disctask_busy()) {
        int rc = disctask_stop(35000);
        printf("  producer: %s\n", cd_strerror(rc));
    }
    bgm_report();
    bgm_forget();
}

// -- psram -----------------------------------------------------------------

static inline uint32_t lcg(uint32_t x) { return x * 1664525u + 1013904223u; }

// Cached writes are only guaranteed visible through the uncached alias after a
// clean; the pre-clean comparison is reported but not asserted, because flash
// code fetch shares the 16 KiB cache and may have evicted the lines already.
static void psram_cache_check(void)
{
    enum { N = 4096u };
    volatile uint32_t *c = (volatile uint32_t *)PSRAM_XIP_BASE;
    volatile uint32_t *u = (volatile uint32_t *)PSRAM_NOCACHE_BASE;

    for (uint32_t i = 0; i < N / 4u; i++) c[i] = 0xC0FFEE00u + i;

    uint32_t stale = 0;
    for (uint32_t i = 0; i < N / 4u; i++) if (u[i] != 0xC0FFEE00u + i) stale++;
    printf("  cache:    %lu/%lu word(s) not yet written back before clean\n",
           (unsigned long)stale, (unsigned long)(N / 4u));

    xip_cache_clean_range(PSRAM_CACHE_OFF, N);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < N / 4u; i++) if (u[i] != 0xC0FFEE00u + i) bad++;
    if (bad) printf("  cache:    FAIL - %lu word(s) still wrong at %08X after clean_range\n",
                    (unsigned long)bad, PSRAM_NOCACHE_BASE);
    else     printf("  cache:    clean_range ok - %08X matches %08X\n",
                    PSRAM_NOCACHE_BASE, PSRAM_XIP_BASE);
}

static void do_psram(const char *arg)
{
    if (!psram_bytes) { printf("  psram: not detected\n"); return; }

    size_t size = *arg ? (size_t)strtoul(arg, NULL, 0) : (1u << 20);
    if (size > psram_bytes) size = psram_bytes;
    size &= ~3u;
    if (size == 0) { printf("  usage: P [bytes]\n"); return; }

    printf("  psram:    %u MiB @ %08X, clkdiv %lu (SCK %lu MHz), rxdelay %lu\n",
           (unsigned)(psram_bytes >> 20), PSRAM_XIP_BASE,
           (unsigned long)psram_divisor,
           (unsigned long)(clock_get_hz(clk_sys) / psram_divisor / 1000000u),
           (unsigned long)psram_rxdelay);

    volatile uint32_t *p = (volatile uint32_t *)PSRAM_XIP_BASE;
    size_t words = size / 4u;

    uint32_t x = 0x12345678u;
    absolute_time_t t0 = get_absolute_time();
    for (size_t i = 0; i < words; i++) { p[i] = x; x = lcg(x); }
    uint32_t wr_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());

    x = 0x12345678u;
    size_t bad = 0, first = (size_t)-1;
    t0 = get_absolute_time();
    for (size_t i = 0; i < words; i++) {
        if (p[i] != x) { if (!bad) first = i; bad++; }
        x = lcg(x);
    }
    uint32_t rd_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());

    if (bad) printf("  memtest:  FAIL - %u/%u word(s) wrong, first @ %u\n",
                    (unsigned)bad, (unsigned)words, (unsigned)(first * 4u));
    else     printf("  memtest:  PASS - %u KiB round-trip\n", (unsigned)(size >> 10));
    rate_line("wr:", size, wr_us, "");
    rate_line("rd:", size, rd_us, "");

    psram_cache_check();
}

// -- audio out -------------------------------------------------------------

static void do_audio(const char *arg)
{
    (void)arg;
    if (bgm_active()) {
        // Fade rather than hard-cut, and do not put the jingle on top of it.
        bgm_stop();
        printf("  audio: bgm stopped\n");
        return;
    }
    if (audio_is_playing()) {
        uint64_t frames = audio_frames_out();
        int64_t  us     = audio_elapsed_us();
        audio_stop();
        printf("  audio: stopped after %lu frames in %lu ms = %lu Hz\n",
               (unsigned long)frames, (unsigned long)(us / 1000),
               us > 0 ? (unsigned long)(frames * 1000000u / (uint64_t)us) : 0ul);
        return;
    }

    jingle_reset();
    if (!audio_start(jingle_fill)) {
        printf("  audio: no PIO/DMA resources - I2S is inert\n");
        return;
    }
    printf("  audio: jingle playing, %u Hz on pio%u"
           " (LRCK=GP%u BCK=GP%u DIN=GP%u)\n",
           (unsigned)AUDIO_SAMPLE_RATE, (unsigned)audio_pio_index(),
           (unsigned)AUDIO_LRCK_PIN, (unsigned)AUDIO_LRCK_PIN + 1u,
           (unsigned)AUDIO_DIN_PIN);
}

// Full-scale sine, so a level sweep has a reference with the same peak as
// mastered content and no DC for the coupling caps to move.
static void do_tone(const char *arg)
{
    if (bgm_active()) bgm_stop();     // fade, so the swap does not thump
    if (audio_is_playing()) {
        audio_stop();
        printf("  audio: tone stopped\n");
        return;
    }
    if (!audio_init()) {
        printf("  audio: no PIO/DMA resources - I2S is inert\n");
        return;
    }
    char *end = NULL;
    uint32_t hz = (uint32_t)strtoul(arg, &end, 10);
    while (end && *end == ' ') end++;
    char ch = end ? *end : 0;
    tone_reset(hz, ch);
    if (!audio_start(tone_fill)) {
        printf("  audio: could not start\n");
        return;
    }
    printf("  audio: %lu Hz sine at 0 dBFS, output %d dBFS%s\n",
           (unsigned long)tone_hz(), audio_get_atten_db(),
           ch == 'l' ? ", left only" : ch == 'r' ? ", right only" : "");
}

// Output level, in dB below full scale. Takes effect live, so the level can be
// swept against the amp while a track plays. `V k` hands control to the panel
// pot; setting a level by hand takes it back, since otherwise the knob would
// overwrite the typed value on its next tick.
static void do_level(const char *arg)
{
    if (*arg == 'k') {
        knob_enable(!knob_enabled());
        if (knob_enabled() && knob_raw() == 0)
            printf("  audio: knob reads 0 - check the wiper on GP%u\n",
                   (unsigned)KNOB_PIN);
    } else if (*arg) {
        knob_enable(false);
        audio_set_atten_db((int)strtol(arg, NULL, 10));
    }
    printf("  audio: output %d dBFS, %s", audio_get_atten_db(),
           knob_enabled() ? "knob" : "manual");
    if (knob_enabled())
        printf(" (adc %u, range %d..%d)", (unsigned)knob_raw(),
               KNOB_LOUD_DB, KNOB_QUIET_DB);
    putchar('\n');
}

static void read_line(char *buf, int max)
{
    int n = 0;
    for (;;) {
        usb_link_pump();
        int ch = getchar_timeout_us(1000);
        // Only with the line empty: an insertion must never land in the middle
        // of something being typed.
        if (ch == PICO_ERROR_TIMEOUT) {
            // Also here, not just on the audio tick: with nothing playing the
            // DMA IRQ is not running, and the knob still has to track.
            if (!audio_is_playing()) knob_poll();
            if (n == 0 && task_poll()) printf("cd> ");
            if (n == 0 && media_poll()) printf("cd> ");
            if (n == 0 && cart_poll()) printf("cd> ");
            if (n == 0 && game_poll()) printf("cd> ");
            if (n == 0 && player_poll()) printf("cd> ");
            if (n == 0 && identify[0] && !disctask_busy() && eject_poll())
                printf("cd> ");
            continue;
        }
        if (ch == '\r' || ch == '\n') { putchar('\n'); break; }
        if ((ch == 0x7f || ch == 0x08) && n > 0) { n--; printf("\b \b"); continue; }
        if (ch >= 0x20 && n < max - 1) { buf[n++] = (char)ch; putchar(ch); }
    }
    buf[n] = 0;
}

int main(void)
{
    stdio_init_all();          // UART0 console on GP0(TX)/GP1(RX)
    gpio_init(RUN_LED_PIN);
    gpio_set_dir(RUN_LED_PIN, GPIO_OUT);
    gpio_put(RUN_LED_PIN, 1);
    add_repeating_timer_ms(RUN_LED_MS, run_led_tick, NULL, &run_led_timer);

    // Before anything else touches the QMI: the M1 timing is derived from
    // clk_sys, and lines cached under the old CS1 setup are stale.
    psram_init(PSRAM_CS_PIN);
    xip_cache_invalidate_all();

    if (!ata_bus_init()) printf("ata: no PIO resources - the bus is inert\n");
    // After the ATA bus: that program wants the PIO at GPIOBASE 0, audio wants
    // one at 16, and whichever claims first fixes the base of the instance.
    bool audio_ok = audio_init();
    knob_init();
    knob_enable(true);
    audio_set_tick_hook(knob_poll);
    eject_init();
    sleep_ms(500);             // settle
    help();
    if (psram_bytes) printf("psram: %u MiB @ %08X\n",
                           (unsigned)(psram_bytes >> 20), PSRAM_XIP_BASE);
    else            printf("psram: not detected on CS1 - P is inert\n");

    // A warm reset keeps the booted ROM's identity; chip_reset cannot tell one
    // from a power-on (openocd's reset leaves it at HAD_POR).
    bool warm = boot_keep.magic == BOOT_WARM_MAGIC;
    boot_keep.magic = BOOT_WARM_MAGIC;
    if (warm) booted_crc = boot_keep.crc; else boot_keep.crc = 0;
    printf("reset: %s%s\n", warm ? "warm" : "power-on",
           booted_crc ? ", the disc's ROM is still on the cart" : "");
    if (!audio_ok) printf("audio: no PIO/DMA resources - M is inert\n");

    printf("resetting bus...\n");
    ata_hard_reset();
    do_reset_wait();           // the signature, not !BSY: a floating bus reads
    do_identify();             // 0x7f, which has BSY clear and DRQ set

    // After the ATA bring-up: ata_wait_not_bsy() blocks for up to 31s with
    // nothing pumping the host stack.
    usb_link_init();
    ata_wait_hook = usb_link_pump;   // ATA waits no longer starve the host stack
    if (disctask_init()) printf("core1: cd reader parked, %u MiB of slots\n",
                                (unsigned)(DISC_SLOTS_BYTES >> 20));
    else printf("core1: not started - no psram, tracks are unavailable\n");
    autoplay_due = make_timeout_time_ms(AUTOPLAY_BOOT_MS);

    printf("usb host: waiting for the cart...\n");
    if (usb_link_wait_ready(2000)) {
        const usb_link_state_t *s = usb_link_state();
        printf("  cdc mounted: %04X:%04X \"%s\" (idx %u)\n",
               s->vid, s->pid, s->product, s->cdc_idx);
    } else {
        printf("  no device after 2s - press n for detail\n");
    }

    char line[96];
    for (;;) {
        printf("cd> ");
        read_line(line, sizeof(line));
        if (line[0] == 0) continue;

        char cmd = line[0];
        const char *arg = &line[1];
        while (*arg == ' ') arg++;

        // Every command that touches the drive, while a track has it.
        if (strchr("xiquelkcdbrtARYLJ", cmd) && !bus_free()) continue;

        switch (cmd) {
        case 'h': help(); break;

        case 'x':
            printf("hard reset...\n");
            disc_speed_reset();
            ata_hard_reset();
            do_reset_wait();
            do_identify();
            break;

        case 'i': do_identify(); break;

        case 'q': {
            size_t got = 0;
            int rc = atapi_inquiry(sector, 36, &got);
            if (rc != ATAPI_OK) { report("INQUIRY", rc); break; }
            char s[32];
            memcpy(s, sector + 8, 8);  s[8]  = 0; printf("  vendor:  %s\n", s);
            memcpy(s, sector + 16, 16); s[16] = 0; printf("  product: %s\n", s);
            memcpy(s, sector + 32, 4);  s[4]  = 0; printf("  rev:     %s\n", s);
            printf("  type:    %02x %s\n", sector[0] & 0x1F,
                   ((sector[0] & 0x1F) == 0x05) ? "(CD-ROM)" : "");
            break;
        }

        case 'u': report("TEST UNIT READY", atapi_wait_ready(30000)); break;
        case 'e':
            disc_speed_reset();
            report("eject", atapi_start_stop(ATAPI_SS_EJECT));
            break;
        case 'l': report("load",  atapi_start_stop(ATAPI_SS_LOAD));  break;

        case 'k': {
            uint32_t x = (uint32_t)strtoul(arg, NULL, 10);
            uint16_t kb = x ? (uint16_t)(x * ATAPI_SPEED_1X) : 0xFFFFu;
            printf("set speed %lux (%u KB/s)\n", (unsigned long)x, kb);
            report("SET CD SPEED", atapi_set_cd_speed(kb));
            break;
        }

        case 'c': do_capacity(); break;

        case 'd': {
            uint32_t lba = (uint32_t)strtoul(arg, NULL, 10);
            size_t got = 0;
            printf("read LBA %lu ...\n", (unsigned long)lba);
            int rc = atapi_read10(lba, 1, sector, sizeof(sector), &got);
            if (rc != ATAPI_OK) { report("READ(10)", rc); break; }
            printf("  got %u bytes\n", (unsigned)got);
            hexdump(sector, (int)(got > 64 ? 64 : got));
            break;
        }

        case 'b': {
            char *end = NULL;
            uint32_t lba = (uint32_t)strtoul(arg, &end, 10);
            uint32_t n   = (uint32_t)strtoul(end ? end : "", NULL, 10);
            do_bulk(lba, n);
            break;
        }

        case 'r': do_raw(arg); break;

        case 't': do_timing(arg); break;

        case 'A': do_async((uint32_t)strtoul(arg, NULL, 10)); break;

        case 'R':
            ata_bus_recover();
            printf("  bus forced idle, PIO machines restarted\n");
            show_status();
            break;

        case 's': show_status(); break;

        case 'n': do_usb_state(); break;
        case 'a': do_n8_open(); break;
        case 'o': do_n8_read(arg); break;
        case 'v': do_n8_memtest(arg); break;
        case 'y': do_n8_bench(arg); break;
        case 'E': do_push_info(); break;
        case 'W': do_push_run(); break;

        case 'P': do_psram(arg); break;
        case 'S': do_disc_stats(); break;
        case 'O': do_autoplay(arg); break;
        case 'Y': disc_acquire(); break;
        case 'G': do_game(arg); break;
        case 'C': player_console(arg); break;
        case 'L': do_catalog_info(arg); break;
        case 'J': do_game_load(arg); break;
        case 'M': do_audio(arg); break;
        case 'B': do_bgm(arg); break;
        case 'V': do_level(arg); break;
        case 'T': do_tone(arg); break;

        case 'g': diag_snapshot(); break;
        case 'f': diag_float_scan(); break;
        case 'w': diag_short_scan(); break;
        case 'm': diag_monitor(); break;

        case 'p': {
            uint32_t g = (uint32_t)strtoul(arg, NULL, 10);
            if (g > 47) { printf("  usage: p <gpio 0..47>\n"); break; }
            diag_blink(g, 10);
            eject_init();                  // it leaves the pad driving
            break;
        }

        default:
            printf("  ? unknown '%c' - press h for help\n", cmd);
        }
    }
}
