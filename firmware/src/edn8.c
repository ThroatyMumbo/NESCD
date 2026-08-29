// edn8.c - EverDrive-N8 PRO host protocol.
//
// A mem_wr whose payload stops short leaves the cart MCU eating everything
// after it as commands, recoverable only by a power cycle. Every rule in
// mem_wr_commit() below exists to make that unreachable.

#include <string.h>
#include "pico/stdlib.h"
#include "edn8.h"
#include "usb_link.h"

#define MEM_WR_HDR      13u          // cmd(4) + addr(4) + len(4) + exec(1)
#define MEM_WR_MAX      (8u << 20)
#define MAX_PAD         (1u << 20)
#define STAGE_SIZE      512u
#define WAKE_ZEROS      66u

uint8_t  edn8_last_status;
size_t   edn8_last_short;
uint32_t edn8_bytes_owed;

enum { ST_CLOSED, ST_IDLE, ST_WEDGED };
static uint8_t state = ST_CLOSED;
static bool desynced;
static edn8_ident_t ident;

static uint8_t stage[STAGE_SIZE];

typedef size_t (*producer_fn)(void *ctx, uint8_t *dst, size_t want);

const char *edn8_strerror(int rc)
{
    switch (rc) {
    case EDN8_OK:       return "ok";
    case EDN8_ETIMEOUT: return "timeout";
    case EDN8_ELINK:    return "link down";
    case EDN8_EPROTO:   return "protocol error";
    case EDN8_EPARAM:   return "bad parameter";
    case EDN8_ESTATUS:  return "device error";
    case EDN8_EVERIFY:  return "verify mismatch";
    case EDN8_EWEDGED:  return "cart parser wedged";
    default:            return "unknown error";
    }
}

// The cart's memtest pattern. Returns the state so a long blob can be generated
// or verified in chunks; seed 0x1234 gives f7 e2 2e f3 20 2b 5a 94 ...
uint32_t edn8_pattern(uint8_t *dst, size_t n, uint32_t x)
{
    if (x == 0) x = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        dst[i] = (uint8_t)x;
    }
    return x;
}

static uint32_t tx_deadline_ms(size_t len)
{
    return 1000u + (uint32_t)(len / 16u);   // tolerates anything above 16 KB/s
}

// -- raw byte movement -----------------------------------------------------

// Push every byte or say exactly how many got out. A short usb_link_tx is the
// normal case, not an error.
static int push(const uint8_t *p, size_t n, absolute_time_t end, size_t *out_done)
{
    size_t off = 0;

    while (off < n) {
        size_t w = usb_link_tx(p + off, n - off);
        off += w;
        if (w == 0) {
            if (!usb_link_ready()) { *out_done = off; return EDN8_ELINK; }
            usb_link_tx_drain(0);
            usb_link_pump();
            if (time_reached(end)) { *out_done = off; return EDN8_ETIMEOUT; }
        }
    }
    *out_done = off;
    return EDN8_OK;
}

int edn8_tx(const void *src, size_t n)
{
    size_t done = 0;
    int rc = push(src, n, make_timeout_time_ms(tx_deadline_ms(n)), &done);
    if (rc != EDN8_OK) {
        edn8_last_short = done;
        desynced = true;
        return rc;
    }
    usb_link_tx_flush();
    return EDN8_OK;
}

int edn8_tx_cmd(uint8_t cmd)
{
    const uint8_t f[4] = { '+', (uint8_t)('+' ^ 0xFF), cmd, (uint8_t)(cmd ^ 0xFF) };
    return edn8_tx(f, sizeof(f));
}

int edn8_tx8(uint8_t v) { return edn8_tx(&v, 1); }

int edn8_tx32(uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return edn8_tx(b, sizeof(b));
}

// pyserial's timeout is a per-read idle timeout, so reset the deadline whenever
// anything arrives; the absolute cap only stops a device dribbling forever.
int edn8_rx(void *dst, size_t n, uint32_t idle_ms)
{
    uint8_t *p = dst;
    size_t got = 0;
    // The cap only stops a device dribbling forever, so it must never undercut
    // the idle the caller asked for: menu_install legitimately waits 30 s.
    uint32_t cap_ms = 10000u + (uint32_t)(n / 16u);
    if (cap_ms < idle_ms * 2u) cap_ms = idle_ms * 2u;
    absolute_time_t idle = make_timeout_time_ms(idle_ms);
    absolute_time_t cap  = make_timeout_time_ms(cap_ms);

    while (got < n) {
        size_t k = usb_link_rx(p + got, n - got);
        if (k) {
            got += k;
            idle = make_timeout_time_ms(idle_ms);
            continue;
        }
        if (!usb_link_ready()) { edn8_last_short = got; desynced = true; return EDN8_ELINK; }
        usb_link_pump();
        if (time_reached(idle) || time_reached(cap)) {
            edn8_last_short = got;
            desynced = true;
            return EDN8_ETIMEOUT;
        }
    }
    return EDN8_OK;
}

size_t edn8_rx_pending(void) { return usb_link_rx_pending(); }

static void drain_quiet(void)
{
    absolute_time_t quiet = make_timeout_time_ms(50);
    absolute_time_t cap   = make_timeout_time_ms(1000);
    uint8_t junk[64];

    for (;;) {
        size_t k = usb_link_rx(junk, sizeof(junk));
        if (k) { quiet = make_timeout_time_ms(50); continue; }
        usb_link_pump();
        if (time_reached(quiet) || time_reached(cap)) return;
    }
}

// -- status ----------------------------------------------------------------

static int status_raw(uint8_t *st)
{
    uint8_t r[2];
    int rc = edn8_tx_cmd(EDN8_CMD_STATUS);
    if (rc != EDN8_OK) return rc;
    rc = edn8_rx(r, sizeof(r), EDN8_T_CMD_MS);
    if (rc != EDN8_OK) return rc;
    if (r[1] != EDN8_STATUS_KEY_OLD) { desynced = true; return EDN8_EPROTO; }
    if (st) *st = r[0];
    return EDN8_OK;
}

static int begin_cmd(void)
{
    if (state == ST_WEDGED) return EDN8_EWEDGED;
    if (state != ST_IDLE)   return EDN8_ELINK;
    if (!usb_link_ready())  { state = ST_CLOSED; return EDN8_ELINK; }

    if (desynced) {
        drain_quiet();
        uint8_t s;
        if (status_raw(&s) != EDN8_OK) return EDN8_EPROTO;
        desynced = false;
    }
    return EDN8_OK;
}

int edn8_status(uint8_t *st)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    return status_raw(st);
}

int edn8_check(void)
{
    uint8_t s = 0;
    int rc = edn8_status(&s);
    if (rc != EDN8_OK) return rc;
    if (s != 0) { edn8_last_status = s; return EDN8_ESTATUS; }
    return EDN8_OK;
}

// -- open / identify -------------------------------------------------------

static int identify(void)
{
    uint8_t a[2], b[2], st[2];
    int rc;

    if ((rc = edn8_tx_cmd(EDN8_CMD_STATUS2)) != EDN8_OK) return rc;
    if ((rc = edn8_tx_cmd(EDN8_CMD_STATUS))  != EDN8_OK) return rc;
    if ((rc = edn8_rx(a, sizeof(a), EDN8_T_PROBE_MS)) != EDN8_OK) return rc;
    if (a[0] != EDN8_STATUS_KEY) return EDN8_EPROTO;
    if ((rc = edn8_rx(b, sizeof(b), EDN8_T_PROBE_MS)) != EDN8_OK) return rc;

    ident.protocol = a[1];
    ident.device   = b[0];
    if (ident.protocol == 0x05 || ident.protocol == EDN8_PROTOCOL_ID) {
        uint8_t drain[2];
        if ((rc = edn8_rx(drain, sizeof(drain), EDN8_T_PROBE_MS)) != EDN8_OK) return rc;
    }

    if ((rc = edn8_tx_cmd(EDN8_CMD_STATUS)) != EDN8_OK) return rc;
    if ((rc = edn8_rx(st, sizeof(st), EDN8_T_PROBE_MS)) != EDN8_OK) return rc;
    if (st[1] != EDN8_STATUS_KEY_OLD) return EDN8_EPROTO;

    if (ident.protocol != EDN8_PROTOCOL_ID || ident.device != EDN8_DEVICE_ID)
        return EDN8_EPROTO;
    return EDN8_OK;
}

// 66 zero bytes clear a half-parsed header: the MCU discards anything that is
// not '+' while waiting for one, and 66 outruns the longest header.
static int wake(void)
{
    uint8_t zeros[WAKE_ZEROS];
    memset(zeros, 0, sizeof(zeros));
    if (edn8_tx(zeros, sizeof(zeros)) != EDN8_OK) return EDN8_ELINK;
    usb_link_tx_drain(EDN8_T_CMD_MS);
    sleep_ms(EDN8_WAKE_SETTLE_MS);
    usb_link_rx_purge();
    return EDN8_OK;
}

// Finish any owed payload FIRST -- zeros land in PSRAM if the cart kept running
// and are discarded by the header parser if it reset.
static int pay_owed(void)
{
    if (edn8_bytes_owed == 0) return EDN8_OK;
    if (edn8_bytes_owed > MAX_PAD) return EDN8_EWEDGED;

    uint32_t owed = edn8_bytes_owed;
    absolute_time_t end = make_timeout_time_ms(tx_deadline_ms(owed));
    memset(stage, 0, sizeof(stage));
    while (owed) {
        size_t n = (owed > STAGE_SIZE) ? STAGE_SIZE : owed;
        size_t done = 0;
        int rc = push(stage, n, end, &done);
        owed -= (uint32_t)done;
        edn8_bytes_owed = owed;
        if (rc != EDN8_OK) return rc;
    }
    return EDN8_OK;
}

int edn8_resync(void)
{
    if (!usb_link_ready()) { state = ST_CLOSED; return EDN8_ELINK; }

    int rc = pay_owed();
    if (rc != EDN8_OK) return rc;
    if ((rc = wake()) != EDN8_OK) return rc;

    uint8_t s;
    if (status_raw(&s) != EDN8_OK) return EDN8_EPROTO;
    desynced = false;
    if (state == ST_WEDGED) state = ST_IDLE;
    return EDN8_OK;
}

int edn8_open(uint32_t wait_ms)
{
    if (!usb_link_wait_ready(wait_ms)) return EDN8_ELINK;

    state = ST_IDLE;
    desynced = false;

    int rc = EDN8_EPROTO;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (pay_owed() != EDN8_OK) { state = ST_WEDGED; return EDN8_EWEDGED; }
        if (wake() != EDN8_OK) { state = ST_CLOSED; return EDN8_ELINK; }
        rc = identify();
        if (rc == EDN8_OK) { desynced = false; return EDN8_OK; }
        if (rc == EDN8_ELINK) { state = ST_CLOSED; return rc; }
        sleep_ms(EDN8_RETRY_MS);
    }
    state = ST_IDLE;
    return rc;
}

bool edn8_is_open(void) { return state == ST_IDLE && usb_link_ready(); }

void edn8_close(void) { state = ST_CLOSED; }

int edn8_ident(edn8_ident_t *out)
{
    if (state != ST_IDLE) return EDN8_ELINK;
    if (out) *out = ident;
    return EDN8_OK;
}

// -- sys info --------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int edn8_sys_info(edn8_sysinfo_t *out)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_tx_cmd(EDN8_CMD_SYS_INF)) != EDN8_OK) return rc;
    if ((rc = edn8_rx(out->raw, sizeof(out->raw), EDN8_T_CMD_MS)) != EDN8_OK) return rc;

    const uint8_t *b = out->raw;
    out->serial_hi  = rd32(b + 20);
    out->serial_lo  = rd32(b + 24);
    out->boot_ctr   = rd32(b + 28);
    out->game_ctr   = rd32(b + 32);
    out->sw_ver     = rd16(b + 44);
    out->hw_ver     = rd16(b + 46);
    out->boot_ver   = rd16(b + 48);
    out->cart_form  = b[52];
    out->flash_size = 1u << b[55];
    return EDN8_OK;
}

// -- mem_wr ----------------------------------------------------------------

typedef struct { const uint8_t *p; size_t off; } buf_ctx_t;
typedef struct { uint32_t x; } pat_ctx_t;
typedef struct { const uint8_t *a, *b; size_t na, nb, off; } two_ctx_t;

static size_t produce_buf(void *ctx, uint8_t *dst, size_t want)
{
    buf_ctx_t *c = ctx;
    memcpy(dst, c->p + c->off, want);
    c->off += want;
    return want;
}

// Two buffers as one payload, so a caller whose message is a body plus a tail
// still spends exactly one command on it. Total, like the others: the caller's
// length is na + nb, which is precisely what this can deliver.
static size_t produce_two(void *ctx, uint8_t *dst, size_t want)
{
    two_ctx_t *c = ctx;
    size_t n = want;
    while (n) {
        size_t k = n;
        const uint8_t *src;
        if (c->off < c->na) {
            src = c->a + c->off;
            if (k > c->na - c->off) k = c->na - c->off;
        } else {
            src = c->b + (c->off - c->na);
            if (k > c->na + c->nb - c->off) k = c->na + c->nb - c->off;
        }
        memcpy(dst, src, k);
        dst += k;
        c->off += k;
        n -= k;
    }
    return want;
}

static size_t produce_pattern(void *ctx, uint8_t *dst, size_t want)
{
    pat_ctx_t *c = ctx;
    c->x = edn8_pattern(dst, want, c->x);
    return want;
}

// The one place a CMD_MEM_WR header is emitted. Both producers are total
// functions, so "declared but not delivered" cannot be expressed by a caller.
static int mem_wr_commit(uint32_t addr, size_t len, producer_fn produce, void *ctx)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if (len == 0 || len > MEM_WR_MAX) return EDN8_EPARAM;
    if (usb_link_rx_pending() != 0) { desynced = true; return EDN8_EPROTO; }
    if (!usb_link_tx_drain(200)) return EDN8_ELINK;

    uint8_t hdr[MEM_WR_HDR] = {
        '+', (uint8_t)('+' ^ 0xFF), EDN8_CMD_MEM_WR, (uint8_t)(EDN8_CMD_MEM_WR ^ 0xFF),
        (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
        (uint8_t)len,  (uint8_t)(len >> 8),  (uint8_t)(len >> 16),  (uint8_t)(len >> 24),
        0                                    // exec: 0xAA here wedges the parser
    };

    // Header and payload are one stream: whatever is left unsent is owed, and
    // zeros complete a partial header just as well as a partial payload.
    size_t total = MEM_WR_HDR + len, sent = 0;
    absolute_time_t end = make_timeout_time_ms(tx_deadline_ms(total));

    rc = EDN8_OK;
    while (sent < total) {
        size_t want;
        const uint8_t *src;
        if (sent < MEM_WR_HDR) {
            src = hdr + sent;
            want = MEM_WR_HDR - sent;
        } else {
            want = total - sent;
            if (want > STAGE_SIZE) want = STAGE_SIZE;
            produce(ctx, stage, want);
            src = stage;
        }
        size_t done = 0;
        rc = push(src, want, end, &done);
        sent += done;
        if (rc != EDN8_OK) break;
    }

    if (rc == EDN8_OK) {
        usb_link_tx_flush();
        return EDN8_OK;
    }

    edn8_bytes_owed = (uint32_t)(total - sent);
    if (rc == EDN8_ELINK) {
        state = ST_WEDGED;              // cannot finish; only a power cycle helps
        return EDN8_EWEDGED;
    }

    // Stalled but still mounted: pad the declared length out and re-frame.
    edn8_last_short = sent;
    int prc = pay_owed();
    if (prc != EDN8_OK) { state = ST_WEDGED; return EDN8_EWEDGED; }
    desynced = true;
    return EDN8_ETIMEOUT;
}

static int region_ok(uint32_t addr, size_t len)
{
    if ((uint64_t)addr + len > EDN8_ADDR_FCI_CFG) return EDN8_EPARAM;
    return EDN8_OK;
}

int edn8_mem_wr(uint32_t addr, const void *data, size_t len)
{
    int rc = region_ok(addr, len);
    if (rc != EDN8_OK) return rc;
    buf_ctx_t c = { .p = data, .off = 0 };
    return mem_wr_commit(addr, len, produce_buf, &c);
}

int edn8_mem_wr_raw(uint32_t addr, const void *data, size_t len)
{
    buf_ctx_t c = { .p = data, .off = 0 };
    return mem_wr_commit(addr, len, produce_buf, &c);
}

int edn8_fifo_send(const void *data, size_t len)
{
    return edn8_mem_wr_raw(EDN8_ADDR_FCI_FIFO, data, len);
}

int edn8_fifo_send2(const void *a, size_t na, const void *b, size_t nb)
{
    two_ctx_t c = { .a = a, .b = b, .na = na, .nb = nb, .off = 0 };
    return mem_wr_commit(EDN8_ADDR_FCI_FIFO, na + nb, produce_two, &c);
}

// cfg.host_ctl bit 0 clears the cart FIFO's pointers and the stream parser,
// with no CPU involvement: this is what lets a streamer attach to a loader that
// is already running. Held either way for millions of the cart's 50 MHz clocks.
int edn8_host_resync(void)
{
    static const uint8_t on = 1, off = 0;
    int rc = edn8_mem_wr_raw(EDN8_ADDR_HOST_CTL, &on, 1);
    if (rc != EDN8_OK) return rc;
    sleep_ms(50);
    if ((rc = edn8_mem_wr_raw(EDN8_ADDR_HOST_CTL, &off, 1)) != EDN8_OK) return rc;
    sleep_ms(50);
    return EDN8_OK;
}

int edn8_mem_wr_pattern(uint32_t addr, size_t len, uint32_t seed)
{
    int rc = region_ok(addr, len);
    if (rc != EDN8_OK) return rc;
    pat_ctx_t c = { .x = seed ? seed : 0xFFFFFFFFu };
    return mem_wr_commit(addr, len, produce_pattern, &c);
}

// -- mem_rd ----------------------------------------------------------------

static int mem_rd_begin(uint32_t addr, size_t len)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if (len == 0) return EDN8_EPARAM;
    if ((rc = edn8_tx_cmd(EDN8_CMD_MEM_RD)) != EDN8_OK) return rc;
    if ((rc = edn8_tx32(addr)) != EDN8_OK) return rc;
    if ((rc = edn8_tx32((uint32_t)len)) != EDN8_OK) return rc;
    if ((rc = edn8_tx8(0)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;
    return EDN8_OK;
}

int edn8_mem_rd(uint32_t addr, void *dst, size_t len)
{
    int rc = mem_rd_begin(addr, len);
    if (rc != EDN8_OK) return rc;
    return edn8_rx(dst, len, EDN8_T_CMD_MS);
}

// The MCU sends every declared byte whether or not we like them, so a mismatch
// records where and keeps reading.
int edn8_mem_rd_verify(uint32_t addr, size_t len, uint32_t seed, size_t *first_diff)
{
    uint8_t got[STAGE_SIZE], want[STAGE_SIZE];
    uint32_t x = seed ? seed : 0xFFFFFFFFu;
    size_t off = 0, diff = (size_t)-1;

    int rc = mem_rd_begin(addr, len);
    if (rc != EDN8_OK) return rc;

    while (off < len) {
        size_t n = len - off;
        if (n > STAGE_SIZE) n = STAGE_SIZE;
        rc = edn8_rx(got, n, EDN8_T_CMD_MS);
        if (rc != EDN8_OK) return rc;
        x = edn8_pattern(want, n, x);
        if (diff == (size_t)-1 && memcmp(got, want, n) != 0) {
            for (size_t i = 0; i < n; i++)
                if (got[i] != want[i]) { diff = off + i; break; }
        }
        off += n;
    }

    if (first_diff) *first_diff = diff;
    return (diff == (size_t)-1) ? EDN8_OK : EDN8_EVERIFY;
}

// -- SD files --------------------------------------------------------------

static int check_raw(void)
{
    uint8_t s = 0;
    int rc = status_raw(&s);
    if (rc != EDN8_OK) return rc;
    if (s != 0) { edn8_last_status = s; return EDN8_ESTATUS; }
    return EDN8_OK;
}

static int tx_string(const char *s)
{
    size_t n = strlen(s);
    if (n > 0xFFFFu) return EDN8_EPARAM;
    const uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
    int rc = edn8_tx(len, sizeof(len));
    if (rc != EDN8_OK) return rc;
    return edn8_tx(s, n);
}

int edn8_dir_make(const char *path)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_tx_cmd(EDN8_CMD_F_DIR_MK)) != EDN8_OK) return rc;
    if ((rc = tx_string(path)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;

    uint8_t s = 0;
    if ((rc = status_raw(&s)) != EDN8_OK) return rc;
    if (s != 0 && s != EDN8_FS_EXIST) { edn8_last_status = s; return EDN8_ESTATUS; }
    return EDN8_OK;
}

// Ancestors only: "games/rom.nes" makes "games" and stops.
int edn8_make_path(const char *path)
{
    char buf[EDN8_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return EDN8_EPARAM;
    memcpy(buf, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        int rc = edn8_dir_make(buf);
        buf[i] = '/';
        if (rc != EDN8_OK) return rc;
    }
    return EDN8_OK;
}

int edn8_file_open(const char *path, uint8_t mode)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_tx_cmd(EDN8_CMD_F_FOPN)) != EDN8_OK) return rc;
    if ((rc = edn8_tx8(mode)) != EDN8_OK) return rc;
    if ((rc = tx_string(path)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;
    return check_raw();
}

int edn8_file_close(void)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_tx_cmd(EDN8_CMD_F_FCLOSE)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;
    return check_raw();
}

// src == NULL pads with zeros, and then the ack gate becomes a drain: the point
// of a pad is to satisfy a declared length the cart may have stopped answering
// for. Draining is not optional either way -- the cart emits one ack per block
// whether or not we read it, and enough unread acks back the RX fifo up and
// stall the transfer we are trying to finish.
static int fwr_payload(const uint8_t *src, size_t len, size_t *off, absolute_time_t end)
{
    while (*off < len) {
        size_t phase = *off % EDN8_ACK_BLOCK;
        if (phase == 0) {
            if (src) {
                uint8_t ack;
                int rc = edn8_rx(&ack, 1, EDN8_T_CMD_MS);
                if (rc != EDN8_OK) return rc;
                if (ack != 0) { edn8_last_status = ack; return EDN8_ESTATUS; }
            } else {
                usb_link_rx_purge();
            }
        }

        size_t n = EDN8_ACK_BLOCK - phase;
        if (n > len - *off) n = len - *off;

        const uint8_t *p;
        if (src) {
            p = src + *off;
        } else {
            if (n > STAGE_SIZE) n = STAGE_SIZE;
            memset(stage, 0, n);
            p = stage;
        }

        size_t done = 0;
        int rc = push(p, n, end, &done);
        *off += done;
        if (rc != EDN8_OK) return rc;
        usb_link_tx_flush();          // the cart cannot ack a block it has not seen
    }
    return EDN8_OK;
}

// F_FWR declares its length up front, so stopping short owes the cart bytes
// exactly as a truncated mem_wr does -- and the same power-cycle is the only
// way back. pay_owed() cannot serve here because it pushes zeros blind.
int edn8_file_write(const void *src, size_t len)
{
    int rc = begin_cmd();
    if (rc != EDN8_OK) return rc;
    if (len == 0 || len > MEM_WR_MAX) return EDN8_EPARAM;
    if (usb_link_rx_pending() != 0) { desynced = true; return EDN8_EPROTO; }
    if (!usb_link_tx_drain(200)) return EDN8_ELINK;

    if ((rc = edn8_tx_cmd(EDN8_CMD_F_FWR)) != EDN8_OK) return rc;
    if ((rc = edn8_tx32((uint32_t)len)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;

    size_t off = 0;
    rc = fwr_payload(src, len, &off, make_timeout_time_ms(tx_deadline_ms(len)));
    if (rc == EDN8_OK) {
        usb_link_tx_flush();
        return check_raw();
    }

    edn8_last_short = off;

    // A nonzero ack is the cart abandoning the command -- everdrive.c returns
    // on it without finishing the length, so it has stopped reading and a pad
    // would be parsed as commands. Nothing is owed.
    if (rc == EDN8_ESTATUS) { desynced = true; return rc; }

    edn8_bytes_owed = (uint32_t)(len - off);
    if (rc == EDN8_ELINK || len - off > MAX_PAD) {
        state = ST_WEDGED;
        return EDN8_EWEDGED;
    }

    size_t pad = off;
    int prc = fwr_payload(NULL, len, &pad, make_timeout_time_ms(tx_deadline_ms(len - off)));
    edn8_bytes_owed = (uint32_t)(len - pad);
    if (prc != EDN8_OK) { state = ST_WEDGED; return EDN8_EWEDGED; }
    desynced = true;
    return rc;
}

// -- menu ------------------------------------------------------------------

// The menu ROM watches the fifo for '*' and one letter, and answers on the
// ordinary IN pipe.
static int mcmd(char ch)
{
    const uint8_t m[2] = { '*', (uint8_t)ch };
    int rc = edn8_fifo_send(m, sizeof(m));
    if (rc != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;
    return EDN8_OK;
}

// With a loader running instead of the menu this puts two bytes into its stream
// parser and times out; edn8_host_resync() is what clears them.
int edn8_menu_test(void)
{
    uint8_t r = 0;
    int rc = mcmd('t');
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_rx(&r, 1, EDN8_T_CMD_MS)) != EDN8_OK) return rc;
    return (r == 'k') ? EDN8_OK : EDN8_EPROTO;
}

// The path goes down the fifo as two writes, length then bytes, and the reply
// is a status plus the mapper index the menu resolved. The FPGA reconfigures
// inside this call, which is what EDN8_T_INSTALL_MS is for.
int edn8_menu_install(const char *path, uint16_t *map_idx)
{
    size_t n = strlen(path);
    if (n == 0 || n > 0xFFFFu) return EDN8_EPARAM;
    const uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };

    int rc = mcmd('n');
    if (rc != EDN8_OK) return rc;
    if ((rc = edn8_fifo_send(len, sizeof(len))) != EDN8_OK) return rc;
    if ((rc = edn8_fifo_send(path, n)) != EDN8_OK) return rc;
    if (!usb_link_tx_drain(EDN8_T_CMD_MS)) return EDN8_ELINK;

    uint8_t st = 0, idx[2];
    if ((rc = edn8_rx(&st, 1, EDN8_T_INSTALL_MS)) != EDN8_OK) return rc;
    if (st != 0) { edn8_last_status = st; return EDN8_ESTATUS; }
    if ((rc = edn8_rx(idx, sizeof(idx), EDN8_T_CMD_MS)) != EDN8_OK) return rc;
    if (map_idx) *map_idx = (uint16_t)(idx[0] | (idx[1] << 8));
    return EDN8_OK;
}

int edn8_menu_start(void) { return mcmd('s'); }

// -- bring-up checks -------------------------------------------------------

int edn8_memtest(size_t size, edn8_rt_result_t out[2])
{
    const uint32_t addrs[2] = { EDN8_ADDR_CHR + 0x1000u, EDN8_ADDR_PRG + 0x20000u };
    int first_rc = EDN8_OK;

    for (int i = 0; i < 2; i++) {
        edn8_rt_result_t *r = &out[i];
        r->addr = addrs[i];
        r->size = size;
        r->first_diff = (size_t)-1;
        r->ok = false;

        r->rc = edn8_mem_wr_pattern(r->addr, size, r->addr);
        if (r->rc == EDN8_OK)
            r->rc = edn8_mem_rd_verify(r->addr, size, r->addr, &r->first_diff);
        r->ok = (r->rc == EDN8_OK);
        if (!r->ok && first_rc == EDN8_OK) first_rc = r->rc;
    }
    return first_rc;
}

int edn8_bench(uint32_t addr, size_t size, bool do_read, edn8_bench_result_t *out)
{
    absolute_time_t t0;
    uint8_t s;

    memset(out, 0, sizeof(*out));
    out->size = size;
    out->first_diff = (size_t)-1;

    t0 = get_absolute_time();
    out->rc = edn8_mem_wr_pattern(addr, size, 0x1234);
    if (out->rc != EDN8_OK) return out->rc;
    if (!usb_link_tx_drain(tx_deadline_ms(size))) { out->rc = EDN8_ELINK; return out->rc; }
    out->wr_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());

    // The parser is sequential, so a STATUS reply cannot arrive until the whole
    // payload has been consumed: this is the only true end-to-end number.
    out->rc = edn8_status(&s);
    if (out->rc != EDN8_OK) return out->rc;
    out->wr_barrier_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());

    if (do_read) {
        t0 = get_absolute_time();
        out->rc = edn8_mem_rd_verify(addr, size, 0x1234, &out->first_diff);
        out->rd_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
        out->match = (out->rc == EDN8_OK);
        if (out->rc == EDN8_EVERIFY) out->rc = EDN8_OK;   // reported via match
    }
    return out->rc;
}
