// eject.c - debounce the panel button and toggle the tray.

#include "eject.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "atapi.h"
#include "disc.h"

#define EJECT_TICK_MS 5

static repeating_timer_t tick_timer;
static bool ready;
static volatile bool armed, saw_low, down, pressed;
static uint8_t low_ms, high_ms;

// Sampled from a timer IRQ, so a press made while the main loop blocks still
// counts, and a pad edge alone never does: ESD off a flying lead is nanoseconds.
static bool __not_in_flash_func(eject_tick)(repeating_timer_t *t)
{
    (void)t;
    if (gpio_is_dir_out(EJECT_PIN)) {      // 'p 33' owns the pad
        down = true;
        low_ms = high_ms = 0;
        return true;
    }
    if (gpio_get(EJECT_PIN)) {
        low_ms = 0;
        if (down && (high_ms += EJECT_TICK_MS) >= EJECT_DEBOUNCE_MS) {
            down = false;
            armed = true;
        }
    } else {
        high_ms = 0;
        if (!armed) saw_low = true;
        if (!down && (low_ms += EJECT_TICK_MS) >= EJECT_PRESS_MS) {
            down = true;
            pressed = true;
        }
    }
    return true;
}

// Re-runnable, and 'p <gpio>' is why: that diagnostic hands the pad to SIO as
// an output and does not give it back.
void eject_init(void)
{
    // Only a pad something drove has presses to drop; a real one stays latched.
    bool driven = !ready || gpio_get_function(EJECT_PIN) != GPIO_FUNC_SIO ||
                  gpio_is_dir_out(EJECT_PIN);
    gpio_init(EJECT_PIN);
    gpio_set_dir(EJECT_PIN, GPIO_IN);
    gpio_pull_up(EJECT_PIN);
    sleep_us(100);                         // let the pull-up take the pad
    if (driven) { down = true; pressed = false; }
    if (ready) return;
    ready = true;
    add_repeating_timer_ms(EJECT_TICK_MS, eject_tick, NULL, &tick_timer);
}

// GESN's door bit first: some drives report 2/3A/00 open and shut alike. The
// ASCQ test (02 open, 00/01 shut) is for a drive that refuses GESN.
static bool tray_is_open(void)
{
    bool open;
    if (atapi_tray_open(&open) == ATAPI_OK) return open;
    if (atapi_test_unit_ready() == ATAPI_OK) return false;
    return atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A &&
           atapi_sense_ascq == 0x02;
}

bool eject_pressed(void)
{
    if (!ready) return false;

    // A pad low at boot is a held button or the reset leaving GP33 unpulled,
    // not a press - reflashing would work the tray every time.
    if (armed && saw_low) {
        saw_low = false;
        printf("\n  button: GP%u was low at boot - tray untouched\n",
               (unsigned)EJECT_PIN);
    }
    if (!pressed) return false;
    pressed = false;
    return true;
}

bool eject_poll(void)
{
    return eject_pressed() && eject_toggle();
}

bool eject_toggle(void)
{
    bool load = tray_is_open();
    disc_speed_reset();                    // whatever comes next is a new disc
    int rc = atapi_start_stop(load ? ATAPI_SS_LOAD : ATAPI_SS_EJECT);
    const char *what = load ? "load" : "eject";
    if (rc == ATAPI_OK) { printf("\n  button: %s ok\n", what); return true; }
    printf("\n  button: %s %s", what, atapi_strerror(rc));
    if (rc == ATAPI_ECHECK)
        printf(" - sense %x/%02x/%02x (%s)", atapi_sense_key, atapi_sense_asc,
               atapi_sense_ascq, atapi_sense_text(atapi_sense_key));
    printf("\n");
    return true;
}
