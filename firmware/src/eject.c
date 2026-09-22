// eject.c - debounce the panel button and toggle the tray.

#include "eject.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "atapi.h"
#include "disc.h"

static bool ready, armed, saw_low, down, was_high;
static absolute_time_t high_since;

// Re-runnable, and 'p <gpio>' is why: that diagnostic hands the pad to SIO as
// an output and does not give it back.
void eject_init(void)
{
    // Only a pad something drove has edges to drop; a real press stays latched.
    bool driven = !ready || gpio_get_function(EJECT_PIN) != GPIO_FUNC_SIO ||
                  gpio_is_dir_out(EJECT_PIN);
    gpio_init(EJECT_PIN);
    gpio_set_dir(EJECT_PIN, GPIO_IN);
    gpio_pull_up(EJECT_PIN);
    sleep_us(100);                         // let the pull-up take the pad
    if (driven) gpio_acknowledge_irq(EJECT_PIN, GPIO_IRQ_EDGE_FALL);
    if (ready) return;
    was_high = false;
    ready = true;
}

// INTR latches the edge whether or not the IRQ is enabled, so a press made
// while the main loop blocks is still there when it next polls.
static bool fell(void)
{
    return (io_bank0_hw->intr[EJECT_PIN / 8] >> (4 * (EJECT_PIN % 8)))
           & GPIO_IRQ_EDGE_FALL;
}

static void clear_fell(void)
{
    gpio_acknowledge_irq(EJECT_PIN, GPIO_IRQ_EDGE_FALL);
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

    absolute_time_t t = get_absolute_time();
    bool now = gpio_get(EJECT_PIN);
    if (now && !was_high) high_since = t;
    was_high = now;
    bool settled = now && absolute_time_diff_us(high_since, t)
                          >= EJECT_DEBOUNCE_MS * 1000;

    // A pad low when polling starts is the reset leaving GP33 unpulled, not a
    // press - reflashing would work the tray every time. Wait for a release.
    if (!armed) {
        if (!now) saw_low = true;
        if (!settled) return false;
        armed = true;
        clear_fell();
        if (saw_low) printf("\n  button: GP%u was low at boot - tray untouched\n",
                            (unsigned)EJECT_PIN);
        return false;
    }

    // Held, or bouncing on release: no new press until it has settled high.
    if (down) {
        if (settled) { down = false; clear_fell(); }
        return false;
    }
    if (!fell()) return false;
    clear_fell();
    down = true;
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
