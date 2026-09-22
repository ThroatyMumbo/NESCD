// ata.c - see ata.h. ATA taskfile for the RP2350B with the data path in PIO.
// Two state machines own DD0..DD15 and one strobe each; the CPU keeps the
// address lines and moves them in one masked store. Reads land in memory by
// DMA when the destination allows it.

#include "ata.h"
#include <string.h>
#include <stdio.h>
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "ata.pio.h"

uint64_t ata_stat_words;
uint64_t ata_stat_bus_us;
void (*ata_wait_hook)(void);

#define STROBE_MASK  ((1u << PIN_DIOR) | (1u << PIN_DIOW))
#define BUS_MASK     (DD_MASK | STROBE_MASK)

// A 1024-word sector burst is 0.6ms at Mode 0, so anything past this is a
// wedged machine rather than a slow one.
#define BUS_TIMEOUT_MS 50u

static PIO      ata_pio;
static uint     sm_rd, sm_wr, off_rd, off_wr;
static int      dma_rd = -1;
static bool     pio_ready;
static bool     bus_held;              // true while diag.c owns the pins
static uint32_t rd_sink;               // DMA target for discarded data

// ---- timing -------------------------------------------------------------

// ATA-4 table 14 minima in ns. t2i (recovery) only exists for modes 3 and 4,
// which need IORDY and are not offered.
typedef struct { uint16_t t0, t1, t2, t3, t4; } ata_mode_t;

static const ata_mode_t MODES[ATA_PIO_MODE_MAX + 1] = {
    { 600, 70, 165, 60, 30 },
    { 383, 50, 125, 45, 20 },
    { 240, 30, 100, 30, 15 },
};

static uint     cur_mode;
static uint32_t cur_tick;              // sysclk cycles per PIO tick
static uint32_t cur_cycle_ns;
static uint8_t  d_rd[3], d_wr[4];      // patched delay slots, for reporting

// ---- address lines (still SIO) ------------------------------------------

static inline void addr_set(uint cs, uint da)
{
    uint32_t v = ((da & 7u) << PIN_DA0)
               | ((cs == ATA_CS_CMD ? 0u : 1u) << PIN_CS0)
               | ((cs == ATA_CS_CTL ? 0u : 1u) << PIN_CS1);
    gpio_put_masked(ATA_ADDR_MASK, v);
}

static inline void addr_idle(void)
{
    gpio_put_masked(ATA_ADDR_MASK, (1u << PIN_CS0) | (1u << PIN_CS1));
}

void ata_gpio_init(void)
{
    // Data bus and strobes idle under SIO first: a failed PIO bring-up then
    // leaves a sane bus rather than whatever the pads powered up as.
    for (uint i = 0; i < 16; i++) { gpio_init(PIN_DD0 + i); gpio_set_dir(PIN_DD0 + i, GPIO_IN); }

    const uint outs[] = { PIN_DA0, PIN_DA1, PIN_DA2, PIN_CS0, PIN_CS1,
                          PIN_DIOR, PIN_DIOW, PIN_RESET, PIN_DMACK };
    for (uint i = 0; i < sizeof(outs)/sizeof(outs[0]); i++) {
        gpio_init(outs[i]); gpio_set_dir(outs[i], GPIO_OUT);
    }
    gpio_put(PIN_DA0, 0); gpio_put(PIN_DA1, 0); gpio_put(PIN_DA2, 0);
    gpio_put(PIN_CS0, 1); gpio_put(PIN_CS1, 1);
    gpio_put(PIN_DIOR, 1); gpio_put(PIN_DIOW, 1);
    gpio_put(PIN_DMACK, 1);
    gpio_put(PIN_RESET, 1);

    const uint ins[] = { PIN_INTRQ, PIN_IORDY, PIN_DMARQ };
    for (uint i = 0; i < sizeof(ins)/sizeof(ins[0]); i++) {
        gpio_init(ins[i]); gpio_set_dir(ins[i], GPIO_IN);
    }
}

// ---- PIO plumbing -------------------------------------------------------

// Both programs must live in the same block: the pin output-value and
// output-enable registers are per-PIO, not per-state-machine, and a pad's
// funcsel picks one instance.
static bool pio_claim_all(void)
{
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &ata_rd_program, &ata_pio, &sm_rd, &off_rd,
            PIN_DD0, PIN_DIOW - PIN_DD0 + 1, true))
        return false;

    int s = pio_claim_unused_sm(ata_pio, false);
    if (s < 0) return false;
    sm_wr  = (uint)s;
    off_wr = pio_add_program(ata_pio, &ata_wr_program);

    dma_rd = dma_claim_unused_channel(false);
    return true;
}

// Idle levels go into the PIO output registers before any pad switches
// funcsel, so no strobe glitches low on the way in.
static void pio_park_pins(void)
{
    pio_sm_set_pins_with_mask(ata_pio, sm_rd, STROBE_MASK, BUS_MASK);
    pio_sm_set_pindirs_with_mask(ata_pio, sm_rd, STROBE_MASK, BUS_MASK);
}

static void pio_configure(void)
{
    pio_sm_config c = ata_rd_program_get_default_config(off_rd);
    sm_config_set_in_pin_base(&c, PIN_DD0);
    sm_config_set_in_pin_count(&c, 16);
    sm_config_set_out_pins(&c, PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, PIN_DIOR);
    sm_config_set_in_shift(&c, true, true, 32);    // right, autopush, 32
    sm_config_set_out_shift(&c, true, false, 32);  // autopull OFF: pull must block
    pio_sm_init(ata_pio, sm_rd, off_rd, &c);

    c = ata_wr_program_get_default_config(off_wr);
    sm_config_set_in_pin_base(&c, PIN_DD0);
    sm_config_set_in_pin_count(&c, 16);
    sm_config_set_out_pins(&c, PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, PIN_DIOW);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_out_shift(&c, true, true, 32);   // right, autopull, 32
    pio_sm_init(ata_pio, sm_wr, off_wr, &c);
}

// INSTR_MEM is write-only, so a delay change re-emits whole instructions from
// the pioasm output. jmp targets are relocated the way pio_add_program does.
static void emit(uint off, const uint16_t *src, uint len, const uint8_t *dly)
{
    for (uint i = 0; i < len; i++) {
        uint16_t v = src[i];
        if ((v & 0xE000u) == 0x0000u) v = (uint16_t)(v + off);
        if (dly[i] != 0xFFu)
            v = (uint16_t)((v & (uint16_t)~0x0F00u) | ((uint16_t)dly[i] << 8));
        ata_pio->instr_mem[off + i] = v;
    }
}

static void reload_programs(void)
{
    uint8_t rd[7], wr[9];
    memset(rd, 0xFF, sizeof(rd));
    memset(wr, 0xFF, sizeof(wr));
    rd[ata_rd_I_SETUP] = d_rd[0];
    rd[ata_rd_I_LOW]   = d_rd[1];
    rd[ata_rd_I_HIGH]  = d_rd[2];
    wr[ata_wr_I_SETUP] = d_wr[0];
    wr[ata_wr_I_DSU]   = d_wr[1];
    wr[ata_wr_I_LOW]   = d_wr[2];
    wr[ata_wr_I_HIGH]  = d_wr[3];

    uint32_t mask = (1u << sm_rd) | (1u << sm_wr);
    pio_set_sm_mask_enabled(ata_pio, mask, false);

    emit(off_rd, ata_rd_program_instructions, count_of(ata_rd_program_instructions), rd);
    emit(off_wr, ata_wr_program_instructions, count_of(ata_wr_program_instructions), wr);

    pio_sm_set_clkdiv_int_frac8(ata_pio, sm_rd, cur_tick, 0);
    pio_sm_set_clkdiv_int_frac8(ata_pio, sm_wr, cur_tick, 0);

    pio_sm_clear_fifos(ata_pio, sm_rd);
    pio_sm_clear_fifos(ata_pio, sm_wr);
    pio_sm_restart(ata_pio, sm_rd);        pio_sm_restart(ata_pio, sm_wr);
    pio_sm_clkdiv_restart(ata_pio, sm_rd); pio_sm_clkdiv_restart(ata_pio, sm_wr);
    pio_sm_exec(ata_pio, sm_rd, pio_encode_jmp(off_rd));
    pio_sm_exec(ata_pio, sm_wr, pio_encode_jmp(off_wr));

    pio_set_sm_mask_enabled(ata_pio, mask, true);
}

// ---- timing solver ------------------------------------------------------

static inline uint32_t ns_cyc(uint32_t ns, uint32_t f)
{
    return (uint32_t)(((uint64_t)ns * f + 999999999u) / 1000000000u);
}

static inline uint32_t up(uint32_t cyc, uint32_t tick) { return (cyc + tick - 1) / tick; }

// Ticks are whole sysclk cycles, so the divider never dithers. Each phase is
// rounded up to its ATA minimum and the cycle is whatever that sums to: a
// phase is never shortened to hit t0, because too-fast timing corrupts data
// silently instead of failing.
uint32_t ata_set_pio_mode(uint mode)
{
    if (mode > ATA_PIO_MODE_MAX || !pio_ready) return cur_cycle_ns;

    const ata_mode_t *m = &MODES[mode];
    uint32_t f = clock_get_hz(clk_sys);
    uint32_t t0c = ns_cyc(m->t0, f);

    // Start at the shortest tick that keeps the cycle inside 20 ticks, then
    // stretch it until every delay slot fits the 4 bits one side-set pin
    // leaves behind.
    for (uint32_t tick = (t0c + 19u) / 20u; tick >= 1u && tick <= 64u; tick++) {
        uint32_t T     = up(t0c, tick);
        uint32_t setup = up(ns_cyc(m->t1, f), tick); if (setup < 1) setup = 1;
        uint32_t low   = up(ns_cyc(m->t2, f), tick); if (low   < 1) low   = 1;
        uint32_t high  = (T > low + 2) ? T - low : 2;           // in + jmp
        uint32_t su    = up(ns_cyc(m->t3, f), tick); if (su    < 1) su    = 1;
        uint32_t whigh = up(ns_cyc(m->t4, f), tick); if (whigh < 1) whigh = 1;
        if (su + low + whigh < T) whigh = T - su - low;

        if (setup - 1 > 15 || low - 1 > 15 || high - 2 > 15 ||
            su - 1 > 15 || whigh - 1 > 15) continue;

        d_rd[0] = (uint8_t)(setup - 1);
        d_rd[1] = (uint8_t)(low - 1);
        d_rd[2] = (uint8_t)(high - 2);
        d_wr[0] = (uint8_t)(setup - 1);
        d_wr[1] = (uint8_t)(su - 1);
        d_wr[2] = (uint8_t)(low - 1);
        d_wr[3] = (uint8_t)(whigh - 1);

        cur_mode     = mode;
        cur_tick     = tick;
        cur_cycle_ns = (uint32_t)(((uint64_t)(low + high) * tick * 1000000000u) / f);
        reload_programs();
        return cur_cycle_ns;
    }
    return cur_cycle_ns;
}

uint     ata_get_pio_mode(void) { return cur_mode; }
uint32_t ata_get_cycle_ns(void) { return cur_cycle_ns; }

void ata_timing_report(void)
{
    uint32_t f = clock_get_hz(clk_sys);
    uint32_t tn = (uint32_t)(((uint64_t)cur_tick * 1000000000u) / f);
    const ata_mode_t *m = &MODES[cur_mode];

    printf("  ATA PIO mode %u: tick %lu cyc (%lu ns), read cycle %lu ns (t0 min %u)\n",
           cur_mode, (unsigned long)cur_tick, (unsigned long)tn,
           (unsigned long)cur_cycle_ns, m->t0);
    printf("    read : t1 %lu ns, t2 %lu ns, recovery %lu ns\n",
           (unsigned long)((d_rd[0] + 1) * tn), (unsigned long)((d_rd[1] + 1) * tn),
           (unsigned long)((d_rd[2] + 2) * tn));
    printf("    write: t3 %lu ns, t2 %lu ns, t4+recovery %lu ns\n",
           (unsigned long)((d_wr[1] + 1) * tn), (unsigned long)((d_wr[2] + 1) * tn),
           (unsigned long)((d_wr[3] + 1) * tn));
    printf("    bus ceiling %lu KB/s\n",
           (unsigned long)(cur_cycle_ns ? 2000000u / cur_cycle_ns : 0u));
}

// ---- bus ownership ------------------------------------------------------

void ata_bus_take(void)
{
    if (!pio_ready) return;
    pio_park_pins();
    for (uint p = PIN_DD0; p < PIN_DD0 + 16; p++) pio_gpio_init(ata_pio, p);
    pio_gpio_init(ata_pio, PIN_DIOR);
    pio_gpio_init(ata_pio, PIN_DIOW);
    bus_held = false;
    ata_bus_recover();
}

void ata_bus_release(void)
{
    if (!pio_ready || bus_held) return;
    bus_held = true;
    for (uint p = PIN_DD0; p < PIN_DD0 + 16; p++) {
        gpio_set_function(p, GPIO_FUNC_SIO);
        gpio_set_dir(p, GPIO_IN);
    }
    gpio_set_function(PIN_DIOR, GPIO_FUNC_SIO);
    gpio_set_function(PIN_DIOW, GPIO_FUNC_SIO);
    gpio_put(PIN_DIOR, 1); gpio_put(PIN_DIOW, 1);
    gpio_set_dir(PIN_DIOR, GPIO_OUT); gpio_set_dir(PIN_DIOW, GPIO_OUT);
}

void ata_bus_recover(void)
{
    if (!pio_ready) return;
    uint32_t mask = (1u << sm_rd) | (1u << sm_wr);

    if (dma_rd >= 0) dma_channel_abort((uint)dma_rd);
    pio_set_sm_mask_enabled(ata_pio, mask, false);
    pio_park_pins();
    reload_programs();
    addr_idle();
}

// ---- transfer primitives ------------------------------------------------

// Done when the machine is back at its idle pull with nothing left to feed
// it. Sticky FIFO flags cannot be used here: a parked machine re-raises
// TXSTALL every cycle, so clearing it races the kick.
static bool wait_park(uint sm, uint off)
{
    absolute_time_t end = make_timeout_time_ms(BUS_TIMEOUT_MS);
    for (;;) {
        if (pio_sm_is_tx_fifo_empty(ata_pio, sm) && pio_sm_get_pc(ata_pio, sm) == off)
            return true;
        if (time_reached(end)) { ata_bus_recover(); return false; }
    }
}

// Two samples pack into one FIFO entry at a 32-bit threshold, first word in
// the low half - a little-endian uint16_t[] exactly. Below 32 a right-shifted
// residue sits in the high half instead, which is how an odd count avoids
// needing a flush.
static inline void rd_thresh(uint bits)
{
    hw_write_masked(&ata_pio->sm[sm_rd].shiftctrl,
                    (bits >= 32u ? 0u : bits) << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB,
                    PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS);
}

static void rd_dma_arm(void *dst, bool increment, size_t entries)
{
    dma_channel_config c = dma_channel_get_default_config((uint)dma_rd);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, increment);
    channel_config_set_dreq(&c, pio_get_dreq(ata_pio, sm_rd, false));
    dma_channel_configure((uint)dma_rd, &c, dst, &ata_pio->rxf[sm_rd], entries, true);
}

// DMA_SIZE_32 ignores the low address bits, and atapi.c hands out buf + done
// with done merely even - an 18-byte REQUEST SENSE lands on a 2 mod 4 offset.
#define RD_FAST_OK(dst, words) \
    ((((uintptr_t)(dst) & 3u) == 0) && (((words) & 1u) == 0) && (words) >= 2)

// dst NULL discards. The CPU fallback moves data at the same rate the DMA
// path does - at 600ns a word the pop loop is a couple of percent duty - so
// it costs occupancy, never throughput.
static void rd_run(uint16_t *dst, size_t words)
{
    if (words == 0) return;

    if (words & 1u) {
        rd_thresh(16);
        pio_sm_put(ata_pio, sm_rd, (uint32_t)(words - 1));
        for (size_t i = 0; i < words; i++) {
            uint32_t v = pio_sm_get_blocking(ata_pio, sm_rd);
            if (dst) dst[i] = (uint16_t)(v >> 16);
        }
    } else {
        size_t pairs = words >> 1;
        rd_thresh(32);
        if (dma_rd >= 0 && (dst == NULL || RD_FAST_OK(dst, words))) {
            rd_dma_arm(dst ? (void *)dst : (void *)&rd_sink, dst != NULL, pairs);
            pio_sm_put(ata_pio, sm_rd, (uint32_t)(words - 1));
            dma_channel_wait_for_finish_blocking((uint)dma_rd);
        } else {
            pio_sm_put(ata_pio, sm_rd, (uint32_t)(words - 1));
            for (size_t i = 0; i < pairs; i++) {
                uint32_t v = pio_sm_get_blocking(ata_pio, sm_rd);
                if (!dst) continue;                  // no DMA channel, discarding
                dst[2 * i]     = (uint16_t)v;
                dst[2 * i + 1] = (uint16_t)(v >> 16);
            }
        }
    }
    wait_park(sm_rd, off_rd);
}

static void wr_run(const uint16_t *src, size_t words)
{
    if (words == 0) return;
    pio_sm_put(ata_pio, sm_wr, (uint32_t)(words - 1));
    for (size_t i = 0; i < words; i += 2) {
        uint32_t v = src[i];
        if (i + 1 < words) v |= (uint32_t)src[i + 1] << 16;
        pio_sm_put_blocking(ata_pio, sm_wr, v);
    }
    wait_park(sm_wr, off_wr);
}

// ---- public bus API -----------------------------------------------------

uint8_t ata_reg_read8(uint cs, uint da)
{
    uint16_t v = 0;
    addr_set(cs, da);
    rd_run(&v, 1);
    addr_idle();
    return (uint8_t)(v & 0xFFu);
}

void ata_reg_write8(uint cs, uint da, uint8_t v)
{
    uint16_t w = v;                   // DD8..15 drive 0; the drive ignores them
    addr_set(cs, da);
    wr_run(&w, 1);
    addr_idle();
}

uint8_t ata_status(void)    { return ata_reg_read8(ATA_CS_CMD, ATA_REG_STATUS); }
uint8_t ata_altstatus(void) { return ata_reg_read8(ATA_CS_CTL, ATA_REG_ALTSTATUS); }

void ata_read_data_burst(uint16_t *dst, size_t words)
{
    absolute_time_t t0 = get_absolute_time();
    addr_set(ATA_CS_CMD, ATA_REG_DATA);
    rd_run(dst, words);
    addr_idle();
    ata_stat_words  += words;
    ata_stat_bus_us += (uint64_t)absolute_time_diff_us(t0, get_absolute_time());
}

void ata_skip_data(size_t words)
{
    absolute_time_t t0 = get_absolute_time();
    addr_set(ATA_CS_CMD, ATA_REG_DATA);
    rd_run(NULL, words);
    addr_idle();
    ata_stat_words  += words;
    ata_stat_bus_us += (uint64_t)absolute_time_diff_us(t0, get_absolute_time());
}

void ata_write_data_burst(const uint16_t *src, size_t words)
{
    addr_set(ATA_CS_CMD, ATA_REG_DATA);
    wr_run(src, words);
    addr_idle();
}

// ---- async read ---------------------------------------------------------

static struct { size_t words; bool busy; absolute_time_t t0; } rd_job;

bool ata_read_begin(uint16_t *dst, size_t words)
{
    if (!pio_ready || bus_held || rd_job.busy || dma_rd < 0) return false;
    if (dst == NULL || !RD_FAST_OK(dst, words)) return false;

    rd_job.words = words; rd_job.busy = true; rd_job.t0 = get_absolute_time();
    addr_set(ATA_CS_CMD, ATA_REG_DATA);
    rd_thresh(32);
    rd_dma_arm(dst, true, words >> 1);
    pio_sm_put(ata_pio, sm_rd, (uint32_t)(words - 1));
    return true;
}

bool ata_read_poll(void)
{
    if (!rd_job.busy) return true;
    if (dma_channel_is_busy((uint)dma_rd)) return false;
    return pio_sm_is_tx_fifo_empty(ata_pio, sm_rd)
        && pio_sm_get_pc(ata_pio, sm_rd) == off_rd;
}

void ata_read_end(void)
{
    if (!rd_job.busy) return;
    absolute_time_t end = make_timeout_time_ms(BUS_TIMEOUT_MS);
    while (!ata_read_poll()) {
        if (time_reached(end)) { ata_bus_recover(); break; }
    }
    addr_idle();
    ata_stat_words  += rd_job.words;
    ata_stat_bus_us += (uint64_t)absolute_time_diff_us(rd_job.t0, get_absolute_time());
    rd_job.busy = false;
}

// ---- waits --------------------------------------------------------------

// Poll alternate status so INTRQ is left alone for a future interrupt path.
bool ata_wait_not_bsy(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    for (;;) {
        if (!(ata_altstatus() & ATA_ST_BSY)) return true;
        if (ata_wait_hook) ata_wait_hook();
        if (time_reached(end)) return false;
    }
}

bool ata_wait_drq(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    for (;;) {
        uint8_t st = ata_altstatus();
        if (!(st & ATA_ST_BSY)) {
            if (st & ATA_ST_DRQ) return true;
            if (st & ATA_ST_ERR) return false;
        }
        if (ata_wait_hook) ata_wait_hook();
        if (time_reached(end)) return false;
    }
}

// A packet device idles at status 0x00 and only sets DRDY once it has been
// given a command, so the signature is the only sound presence test.
bool ata_wait_signature(uint32_t timeout_ms)
{
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    for (;;) {
        uint8_t mid = 0, high = 0;
        ata_read_signature(&mid, &high);
        if (mid == 0x14 && high == 0xEB) return ata_wait_not_bsy(1000);
        if (time_reached(end)) return false;
        sleep_ms(50);
    }
}

void ata_read_signature(uint8_t *mid, uint8_t *high)
{
    if (mid)  *mid  = ata_reg_read8(ATA_CS_CMD, ATA_REG_BCOUNT_LO);
    if (high) *high = ata_reg_read8(ATA_CS_CMD, ATA_REG_BCOUNT_HI);
}

// ---- resets and identify ------------------------------------------------

void ata_hard_reset(void)
{
    gpio_put(PIN_RESET, 0);
    busy_wait_us_32(100);             // spec minimum is 25us
    gpio_put(PIN_RESET, 1);
    sleep_ms(50);                     // let the drive start its self-test
}

bool ata_soft_reset(uint32_t timeout_ms)
{
    ata_reg_write8(ATA_CS_CTL, ATA_REG_DEVCTL, ATA_DEVCTL_SRST | ATA_DEVCTL_NIEN);
    busy_wait_us_32(20);
    ata_reg_write8(ATA_CS_CTL, ATA_REG_DEVCTL, ATA_DEVCTL_NIEN);
    busy_wait_us_32(2000);
    return ata_wait_not_bsy(timeout_ms);
}

void ata_select_device(uint dev)
{
    ata_reg_write8(ATA_CS_CMD, ATA_REG_DEVICE, dev ? 0x10u : 0x00u);
    busy_wait_us_32(1);               // 400ns settle before status is valid
}

// SET FEATURES 0xEF/0x03. SECCOUNT 01h is "PIO default mode, disable IORDY",
// which is exactly what this host does; 08h+n asks for flow control mode n and
// entitles the drive to assert IORDY, which nothing here honors. Mode 0 is
// therefore the only request that is strictly in spec. A rejection is not
// fatal - mode 0 is also the power-on default.
bool ata_set_xfer_mode(uint mode)
{
    uint8_t sc = (mode == 0) ? 0x01u : (uint8_t)(0x08u | (mode & 7u));
    if (!ata_wait_not_bsy(5000)) return false;
    ata_select_device(0);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_FEATURES, ATA_FEAT_XFER_MODE);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_SECCOUNT, sc);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_COMMAND,  ATA_CMD_SET_FEATURES);
    if (!ata_wait_not_bsy(5000)) return false;
    return !(ata_status() & (ATA_ST_ERR | ATA_ST_DF));
}

// Zeroed on failure: callers gate on id256[0], and a floating bus bursts 0x7f7f.
bool ata_identify_packet(uint16_t *id256)
{
    memset(id256, 0, 512);
    if (!ata_wait_not_bsy(5000)) return false;
    ata_select_device(0);
    ata_reg_write8(ATA_CS_CMD, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    if (!ata_wait_drq(5000)) return false;
    ata_read_data_burst(id256, 256);
    if (!(ata_status() & ATA_ST_ERR)) return true;
    memset(id256, 0, 512);
    return false;
}

void ata_id_string(const uint16_t *id, int first, int nwords, char *out, int outsz)
{
    int k = 0;
    for (int i = 0; i < nwords && k < outsz - 2; i++) {
        uint16_t w = id[first + i];
        out[k++] = (char)(w >> 8);    // IDENTIFY strings are byte-swapped
        out[k++] = (char)(w & 0xFF);
    }
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0)) k--;
    out[k] = 0;
}

// ---- init ---------------------------------------------------------------

bool ata_bus_init(void)
{
    if (pio_ready) return true;
    ata_gpio_init();
    if (!pio_claim_all()) return false;

    pio_park_pins();
    for (uint p = PIN_DD0; p < PIN_DD0 + 16; p++) pio_gpio_init(ata_pio, p);
    pio_gpio_init(ata_pio, PIN_DIOR);
    pio_gpio_init(ata_pio, PIN_DIOW);

    pio_configure();
    pio_ready = true;
    ata_set_pio_mode(0);
    addr_idle();
    return true;
}
