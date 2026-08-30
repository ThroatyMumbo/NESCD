// eject.c - debounce the panel button and toggle the tray.

#include "eject.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "atapi.h"
#include "disc.h"

static bool ready, raw, state, armed, saw_low;
static absolute_time_t changed;

// Re-runnable, and 'p <gpio>' is why: that diagnostic hands the pad to SIO as
// an output and does not give it back.
void eject_init(void)
{
    gpio_init(EJECT_PIN);
    gpio_set_dir(EJECT_PIN, GPIO_IN);
    gpio_pull_up(EJECT_PIN);
    if (ready) return;
    sleep_us(100);                         // let the pull-up take the pad
    raw = state = gpio_get(EJECT_PIN);     // idle high; pressed is low
    changed = get_absolute_time();
    ready = true;
}

// 2/3A/02 is the tray already open, against 00 and 01 for a shut tray with no
// disc - so the toggle reads ASCQ, where media_state() only needs the ASC.
static bool tray_is_open(void)
{
    if (atapi_test_unit_ready() == ATAPI_OK) return false;
    return atapi_sense_key == 0x02 && atapi_sense_asc == 0x3A &&
           atapi_sense_ascq == 0x02;
}

bool eject_pressed(void)
{
    if (!ready) return false;

    bool now = gpio_get(EJECT_PIN);
    if (now != raw) { raw = now; changed = get_absolute_time(); return false; }
    if (absolute_time_diff_us(changed, get_absolute_time())
        < EJECT_DEBOUNCE_MS * 1000) return false;

    // A pad low when polling starts is the reset leaving GP33 unpulled, not a
    // press - reflashing would work the tray every time. Wait for a release.
    if (!armed) {
        if (!now) { saw_low = true; return false; }
        armed = true;
        state = true;
        if (saw_low) printf("\n  button: GP%u was low at boot - tray untouched\n",
                            (unsigned)EJECT_PIN);
        return false;
    }

    if (now == state) return false;
    state = now;
    return !state;                         // false on the release edge
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
