// diag.c - see diag.h. Bus-level wiring checks that do not need a live drive.

#include "diag.h"
#include "ata.h"
#include <stdio.h>
#include "hardware/gpio.h"

typedef struct {
    uint8_t     gpio;
    uint8_t     ata;        // connector pin number
    const char *name;
    bool        driven;     // true if the host drives this line
} diag_pin_t;

// DD0..DD7 and DD8..DD15 interleave across the connector's two rows, which is
// the mapping most likely to be miswired by hand.
static const diag_pin_t PINS[] = {
    {  2, 17, "DD0",    true }, {  3, 15, "DD1",    true },
    {  4, 13, "DD2",    true }, {  5, 11, "DD3",    true },
    {  6,  9, "DD4",    true }, {  7,  7, "DD5",    true },
    {  8,  5, "DD6",    true }, {  9,  3, "DD7",    true },
    { 10,  4, "DD8",    true }, { 11,  6, "DD9",    true },
    { 12,  8, "DD10",   true }, { 13, 10, "DD11",   true },
    { 14, 12, "DD12",   true }, { 15, 14, "DD13",   true },
    { 16, 16, "DD14",   true }, { 17, 18, "DD15",   true },
    { 18, 35, "DA0",    true }, { 19, 33, "DA1",    true },
    { 20, 36, "DA2",    true },
    { 21, 37, "CS0#",   true }, { 22, 38, "CS1#",   true },
    { 23, 25, "DIOR#",  true }, { 24, 23, "DIOW#",  true },
    { 25,  1, "RESET#", true },
    { 26, 31, "INTRQ",  false }, { 27, 27, "IORDY",  false },
    { 28, 21, "DMARQ",  false }, { 29, 29, "DMACK#", true },
};
#define NPINS (sizeof(PINS) / sizeof(PINS[0]))

// A powered drive holds the bus high through ~10k pull-ups, which swamp the
// RP2350's ~60k internal pulls. Idle high is therefore the reference level.
static void all_inputs_pullup(void)
{
    for (size_t i = 0; i < NPINS; i++) {
        gpio_set_dir(PINS[i].gpio, GPIO_IN);
        gpio_set_pulls(PINS[i].gpio, true, false);
    }
    sleep_us(50);
}

static void restore_idle(void)
{
    for (size_t i = 0; i < NPINS; i++) gpio_set_pulls(PINS[i].gpio, false, false);
    ata_gpio_init();
    ata_bus_take();
}

// gpio_get() reads the pad and is funcsel-independent, but gpio_is_dir_out()
// reads SIO's own output enable and means nothing for a PIO-driven pin.
void diag_snapshot(void)
{
    printf("  line    GPIO  ATA  dir  level\n");
    for (size_t i = 0; i < NPINS; i++) {
        const diag_pin_t *p = &PINS[i];
        const char *dir = (gpio_get_function(p->gpio) != GPIO_FUNC_SIO)
                        ? "pio" : (gpio_is_dir_out(p->gpio) ? "out" : "in");
        printf("  %-7s %-5u %-4u %-4s %u\n", p->name, p->gpio, p->ata,
               dir, gpio_get(p->gpio));
    }
}

void diag_float_scan(void)
{
    ata_bus_release();
    printf("  Pulling each line up then down. FLOAT = nothing on the far end\n"
           "  is driving it (open wire, or a high-Z drive input).\n\n");
    printf("  line    GPIO  ATA  pu pd  verdict\n");

    int floating = 0, driven_lo = 0, driven_hi = 0;
    for (size_t i = 0; i < NPINS; i++) {
        const diag_pin_t *p = &PINS[i];
        gpio_set_dir(p->gpio, GPIO_IN);

        gpio_set_pulls(p->gpio, true, false); sleep_us(200);
        int hi = gpio_get(p->gpio);
        gpio_set_pulls(p->gpio, false, true); sleep_us(200);
        int lo = gpio_get(p->gpio);
        gpio_set_pulls(p->gpio, false, false);

        const char *verdict;
        if (hi && !lo)      { verdict = "FLOAT";        floating++;  }
        else if (hi && lo)  { verdict = "DRIVEN HIGH";  driven_hi++; }
        else if (!hi && !lo){ verdict = "DRIVEN LOW";   driven_lo++; }
        else                 verdict = "NOISY";

        printf("  %-7s %-5u %-4u %u  %u   %s\n", p->name, p->gpio, p->ata, hi, lo, verdict);
    }
    printf("\n  %d floating, %d driven high, %d driven low\n",
           floating, driven_hi, driven_lo);
    if (floating == (int)NPINS)
        printf("  Every line floats: the drive is unpowered, or no grounds are shared.\n");
    restore_idle();
}

void diag_short_scan(void)
{
    ata_bus_release();
    printf("  Driving each host line low in turn; anything else that follows\n"
           "  it low is shorted to it. The bus idles high on a powered drive.\n\n");

    int shorts = 0;
    for (size_t i = 0; i < NPINS; i++) {
        const diag_pin_t *p = &PINS[i];
        if (!p->driven) continue;

        all_inputs_pullup();
        gpio_set_pulls(p->gpio, false, false);
        gpio_set_dir(p->gpio, GPIO_OUT);
        gpio_put(p->gpio, 0);
        sleep_us(500);

        for (size_t j = 0; j < NPINS; j++) {
            if (j == i) continue;
            if (!gpio_get(PINS[j].gpio)) {
                printf("  SHORT: %s (GP%u/ATA%u) -> %s (GP%u/ATA%u)\n",
                       p->name, p->gpio, p->ata,
                       PINS[j].name, PINS[j].gpio, PINS[j].ata);
                shorts++;
            }
        }
        gpio_put(p->gpio, 1);
        gpio_set_dir(p->gpio, GPIO_IN);
    }
    printf("\n  %d coupling(s) found\n", shorts);
    restore_idle();
}

void diag_blink(uint gpio, uint32_t seconds)
{
    const diag_pin_t *p = NULL;
    for (size_t i = 0; i < NPINS; i++) if (PINS[i].gpio == gpio) p = &PINS[i];

    ata_bus_release();
    gpio_init(gpio);

    printf("  toggling GP%u", gpio);
    if (p) printf(" = %s, ATA pin %u", p->name, p->ata);
    printf(" at 2Hz for %lus\n", (unsigned long)seconds);

    gpio_set_dir(gpio, GPIO_OUT);
    for (uint32_t i = 0; i < seconds * 4; i++) {
        gpio_put(gpio, i & 1);
        sleep_ms(250);
    }
    restore_idle();
}

void diag_monitor(void)
{
    printf("  watching all lines - press any key to stop\n");
    uint32_t prev = ~0u;
    for (;;) {
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) break;

        uint32_t now = 0;
        for (size_t i = 0; i < NPINS; i++)
            if (gpio_get(PINS[i].gpio)) now |= 1u << i;

        if (now != prev) {
            printf("  ");
            for (size_t i = 0; i < NPINS; i++)
                printf("%s=%u ", PINS[i].name, (now >> i) & 1u);
            printf("\n");
            prev = now;
        }
        sleep_ms(50);
    }
}
