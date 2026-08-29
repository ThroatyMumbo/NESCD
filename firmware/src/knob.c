// knob.c - read the volume pot and drive audio.c's attenuator.

#include "knob.h"

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "audio.h"

#define EMA_SHIFT   3               // ~93 ms at the 86 Hz tick
#define ADC_MAX     4095u
#define SPAN_DB     (KNOB_LOUD_DB - KNOB_QUIET_DB)
#define DEADBAND    6               // tenths of a dB; 1 dB steps need <10

static bool ready, on;
static uint32_t ema;
static int cur_db;

void knob_init(void)
{
    if (ready) return;
    adc_init();
    adc_gpio_init(KNOB_PIN);
    ready = true;
}

void knob_enable(bool want)
{
    if (!ready || on == want) return;
    on = want;
    if (!on) return;
    // Reclaim the pad: 'p <gpio>' and the other diagnostics hand pins to SIO,
    // and would otherwise leave this one driving into the pot.
    adc_gpio_init(KNOB_PIN);
    adc_select_input(KNOB_ADC_IN);
    ema = adc_read() << EMA_SHIFT;             // adopt the position at once
    cur_db = 0;                                // force the first update
    knob_poll();
}

bool knob_enabled(void)  { return on; }
uint16_t knob_raw(void)  { return (uint16_t)(ema >> EMA_SHIFT); }

void __not_in_flash_func(knob_poll)(void)
{
    if (!on) return;
    adc_select_input(KNOB_ADC_IN);
    int32_t acc = (int32_t)ema;
    acc += (int32_t)adc_read() - (acc >> EMA_SHIFT);
    ema = (uint32_t)(acc < 0 ? 0 : acc);
    uint32_t v = ema >> EMA_SHIFT;
    if (v > ADC_MAX) v = ADC_MAX;

    // Tenths of a dB of attenuation, so the deadband can be finer than a step.
    int tenths = -KNOB_QUIET_DB * 10
               - (int)(v * (uint32_t)(SPAN_DB * 10) / ADC_MAX);
    int delta = tenths - (-cur_db) * 10;
    if (delta > -DEADBAND && delta < DEADBAND) return;

    int att = (tenths + 5) / 10;
    if (att < -KNOB_LOUD_DB) att = -KNOB_LOUD_DB;
    if (att > -KNOB_QUIET_DB) att = -KNOB_QUIET_DB;
    cur_db = -att;
    audio_set_atten_db(cur_db);
}
