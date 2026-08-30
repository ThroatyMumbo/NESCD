// audio.c - I2S transport: two DMA channels chained head-to-tail around a pair
// of buffers, refilled from the completion IRQ.
//
// The refill happens in the ISR rather than a main-loop poll because core0
// can sit inside a long drive or cart operation with nothing polling.

#include "audio.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "audio_i2s.pio.h"

static PIO  pio;
static uint sm;
static uint offset;
static int  dma_chan[2] = { -1, -1 };

static bool ready;
static bool playing;
static audio_fill_fn fill_fn;
static void (*tick_hook)(void);

static uint32_t buf[2][AUDIO_BUF_FRAMES] __attribute__((aligned(4)));

static volatile uint64_t frames_out;
static absolute_time_t   t_start;

// Master attenuation in Q16, slewed rather than stepped: a level discontinuity
// mid-waveform thumps the PCM5102 the same way a DC step at stop does.
#define ATTEN_UNITY (1u << 16)
#define ATTEN_SLEW  64u               // ~23 ms for a full-scale change

static volatile uint32_t atten_tgt = ATTEN_UNITY;
static uint32_t atten_cur = ATTEN_UNITY;
static int      atten_db;

// Exact in int32: 32768 * 65536 is INT32_MIN and 32767 * 65536 fits, so no
// 64-bit multiply is needed for what runs 44100 times a second.
static void __not_in_flash_func(apply_atten)(uint32_t *b, uint n)
{
    uint32_t tgt = atten_tgt;
    if (tgt == ATTEN_UNITY && atten_cur == ATTEN_UNITY) return;

    for (uint i = 0; i < n; i++) {
        if (atten_cur < tgt)
            atten_cur = atten_cur + ATTEN_SLEW > tgt ? tgt : atten_cur + ATTEN_SLEW;
        else if (atten_cur > tgt)
            atten_cur = atten_cur < tgt + ATTEN_SLEW ? tgt : atten_cur - ATTEN_SLEW;
        int32_t r = (int16_t)(b[i] >> 16), l = (int16_t)b[i];   // high half = right slot
        l = (l * (int32_t)atten_cur) >> 16;
        r = (r * (int32_t)atten_cur) >> 16;
        b[i] = ((uint32_t)(uint16_t)(int16_t)r << 16) | (uint16_t)(int16_t)l;
    }
}

// 10^(-dB/20) in Q16, 0 to -40 dB. A table rather than powf because the knob
// calls this from the DMA IRQ.
static const uint32_t atten_q16[AUDIO_ATTEN_MAX + 1] = {   // unity is 65536
    65536, 58409, 52057, 46396, 41350, 36854, 32846, 29274,
    26090, 23253, 20724, 18471, 16462, 14672, 13076, 11654,
    10387,  9257,  8250,  7353,  6554,  5841,  5206,  4640,
     4135,  3685,  3285,  2927,  2609,  2325,  2072,  1847,
     1646,  1467,  1308,  1165,  1039,   926,   825,   735,
      655,
};

void __not_in_flash_func(audio_set_atten_db)(int db)
{
    if (db > 0) db = -db;
    if (db < -AUDIO_ATTEN_MAX) db = -AUDIO_ATTEN_MAX;
    atten_db = db;
    atten_tgt = atten_q16[-db];
    if (!playing) atten_cur = atten_tgt;      // nothing clocked out to ramp
}

int audio_get_atten_db(void) { return atten_db; }

void audio_set_tick_hook(void (*fn)(void)) { tick_hook = fn; }

// In RAM: core0 runs out of flash XIP while this fires, so an instruction
// fetch here would take cache misses that compete with it.
static void __isr __not_in_flash_func(audio_dma_isr)(void)
{
    for (int i = 0; i < 2; i++) {
        if (!(dma_hw->ints0 & (1u << dma_chan[i]))) continue;
        dma_hw->ints0 = 1u << dma_chan[i];
        // The chained channel is already playing the other buffer, so there is
        // a full buffer period to rewind this one and refill it.
        dma_channel_set_read_addr((uint)dma_chan[i], buf[i], false);
        fill_fn(buf[i], AUDIO_BUF_FRAMES);
        apply_atten(buf[i], AUDIO_BUF_FRAMES);
        frames_out += AUDIO_BUF_FRAMES;
        // The knob, when one is fitted: core0 sits in the record loop for
        // minutes at a time, so this is the only thing running during a stream.
        if (tick_hook) tick_hook();
    }
}

bool audio_init(void)
{
    if (ready) return true;

    // The pins straddle GP31, so this needs an instance at GPIOBASE 16 - a
    // different PIO from the ATA bus, which is claimed at base 0.
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &audio_i2s_program, &pio, &sm, &offset,
            AUDIO_GPIO_BASE, AUDIO_GPIO_COUNT, true))
        return false;

    dma_chan[0] = dma_claim_unused_channel(false);
    dma_chan[1] = dma_claim_unused_channel(false);
    if (dma_chan[0] < 0 || dma_chan[1] < 0) return false;

    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_isr);
    // Under the USB host IRQ: a refill can wait 11 ms, but a late EPX re-arm
    // costs cart bandwidth a ROM push is budgeted on.
    irq_set_priority(DMA_IRQ_0, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    // Once, behind the `ready` guard: a later audio_init() must not walk over
    // a level the console or the knob has since set.
    audio_set_atten_db(AUDIO_ATTEN_BOOT_DB);

    ready = true;
    return true;
}

bool audio_start(audio_fill_fn fill)
{
    if (!ready || !fill) return false;
    if (playing) audio_stop();

    fill_fn = fill;
    audio_i2s_program_init(pio, sm, offset, AUDIO_DIN_PIN, AUDIO_LRCK_PIN,
                           AUDIO_SAMPLE_RATE);

    // Prime both buffers before anything is clocked out.
    fill(buf[0], AUDIO_BUF_FRAMES);
    apply_atten(buf[0], AUDIO_BUF_FRAMES);
    fill(buf[1], AUDIO_BUF_FRAMES);
    apply_atten(buf[1], AUDIO_BUF_FRAMES);
    frames_out = 0;

    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config((uint)dma_chan[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
        channel_config_set_chain_to(&c, (uint)dma_chan[i ^ 1]);
        dma_channel_configure((uint)dma_chan[i], &c, &pio->txf[sm], buf[i],
                              AUDIO_BUF_FRAMES, false);
        dma_channel_set_irq0_enabled((uint)dma_chan[i], true);
    }
    dma_hw->ints0 = (1u << dma_chan[0]) | (1u << dma_chan[1]);

    t_start = get_absolute_time();
    playing = true;
    dma_channel_start((uint)dma_chan[0]);
    pio_sm_set_enabled(pio, sm, true);
    return true;
}

void audio_stop(void)
{
    if (!playing) return;
    playing = false;

    // Break the ping-pong before aborting: a channel that still chains would
    // just restart its partner on the way down.
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_get_channel_config((uint)dma_chan[i]);
        channel_config_set_chain_to(&c, (uint)dma_chan[i]);
        dma_channel_set_config((uint)dma_chan[i], &c, false);
        dma_channel_set_irq0_enabled((uint)dma_chan[i], false);
    }
    for (int i = 0; i < 2; i++) dma_channel_abort((uint)dma_chan[i]);
    dma_hw->ints0 = (1u << dma_chan[0]) | (1u << dma_chan[1]);

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    // Park the lines low so the DAC is not left holding a DC level.
    pio_sm_set_pins_with_mask64(pio, sm, 0, AUDIO_PIN_MASK);
}

bool audio_is_playing(void)      { return playing; }
bool audio_fill_is(audio_fill_fn fn) { return playing && fill_fn == fn; }
uint64_t audio_frames_out(void)  { return frames_out; }
uint audio_pio_index(void)       { return ready ? pio_get_index(pio) : 0u; }

void audio_set_clkdiv_x256(uint32_t d)
{
    if (!ready) return;
    if (d < 256u) d = 256u;                       // div < 1 is not representable
    pio_sm_set_clkdiv_int_frac8(pio, sm, (uint16_t)(d >> 8), (uint8_t)(d & 0xFFu));
}

uint32_t audio_get_clkdiv_x256(void)
{
    if (!ready) return 0;
    uint32_t c = pio->sm[sm].clkdiv;
    return ((c >> PIO_SM0_CLKDIV_INT_LSB) & 0xFFFFu) * 256u
         + ((c >> PIO_SM0_CLKDIV_FRAC_LSB) & 0xFFu);
}

uint32_t audio_clkdiv_x256_for_uhz(uint64_t uhz)
{
    // div = clk_sys / (rate * 64), in 16.8: clk_sys * 256 * 1e6 / (64 * uhz).
    if (!uhz) return 0;
    return (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 4000000ull) / uhz);
}

int64_t audio_elapsed_us(void)
{
    if (!playing) return 0;
    return absolute_time_diff_us(t_start, get_absolute_time());
}
