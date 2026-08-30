// disctask.c - core1 runs the CD, core0 runs USB. The split is forced:
// tuh_init() enables its IRQ per core and edn8.c has module state, so the host
// stack stays on core0; ata.c has one in-flight read job and one DMA channel,
// so the drive gets core1 to itself.
//
// Only a start needs the mailbox. Stop is a flag the producer polls at its
// boundaries and inside its reads, so a stop during a multi-second drive stall
// lands as soon as the drive releases BSY.

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "ata.h"
#include "cdcore.h"
#include "disc.h"
#include "disctask.h"
#include "psram.h"
#include "track.h"
#include "usb_link.h"

static volatile uint32_t cmd_gen, ack_gen, cmd_arg;
static const disc_job_t *volatile cmd_job;
static volatile int      cmd_rc;
static bool launched;
static uint32_t reaped_gen;          // last job the idle loop has acted on

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// Core1's wait, both for slot space and inside every ATA BSY/DRQ poll. It must
// never touch TinyUSB: core0 owns that stack and pumps it continuously.
static void core1_wait(void) { busy_wait_us_32(100); }

// Core0's wait while a first open sits out the SET CD SPEED window.
static void core0_pause(uint32_t ms)
{
    absolute_time_t t = make_timeout_time_ms(ms);
    while (!time_reached(t)) { usb_link_pump(); sleep_ms(1); }
}

static void core1_main(void)
{
    for (;;) {
        uint32_t g = __atomic_load_n(&cmd_gen, __ATOMIC_ACQUIRE);
        if (g == ack_gen) { __wfe(); continue; }

        cmd_rc = cmd_job->run(cmd_arg);
        __atomic_store_n(&ack_gen, g, __ATOMIC_RELEASE);
    }
}

bool disctask_init(void)
{
    if (launched) return true;
    if (psram_bytes < (size_t)DISC_SLOTS_OFF + DISC_SLOTS_BYTES) return false;

    disc_slots_attach((uint8_t *)PSRAM_XIP_BASE + DISC_SLOTS_OFF, DISC_SLOTS_BYTES);
    disc_now_ms = now_ms;
    disc_idle   = core1_wait;
    disc_pause_ms = core0_pause;

    multicore_launch_core1(core1_main);
    launched = true;
    return true;
}

bool disctask_ready(void) { return launched; }
bool disctask_busy(void) { return cmd_gen != ack_gen; }

int disctask_run(const disc_job_t *job, uint32_t start_seq)
{
    if (!launched) return CD_EPSRAM;
    if (disctask_busy()) return CD_EDISCIO;
    job->reset();

    // The drive belongs to core1 for the duration, so its waits must stop
    // pumping a host stack that core0 is already driving.
    ata_wait_hook = NULL;

    cmd_arg = start_seq;
    cmd_job = job;                    // ordered by the release on cmd_gen
    reaped_gen = cmd_gen;             // this job is the one to reap next
    __atomic_store_n(&cmd_gen, cmd_gen + 1u, __ATOMIC_RELEASE);
    __sev();
    return CD_OK;
}

int disctask_track(uint32_t start_seq) { return disctask_run(&track_job, start_seq); }

const disc_job_t *disctask_job(void) { return cmd_job; }

int disctask_stop(uint32_t timeout_ms)
{
    if (!launched || !cmd_job) return CD_OK;
    cmd_job->stop();

    // Core1 may be parked in a 30 s BSY wait. Taking the bus back before it
    // returns is the one unrecoverable mistake here, so time out rather than
    // assume.
    absolute_time_t give = make_timeout_time_ms(timeout_ms);
    while (disctask_busy()) {
        usb_link_pump();
        if (absolute_time_diff_us(get_absolute_time(), give) <= 0) return CD_EDISCIO;
        sleep_ms(1);
    }
    ata_wait_hook = usb_link_pump;
    reaped_gen = cmd_gen;
    return cmd_rc;
}

bool disctask_reap(int *rc)
{
    if (!launched || disctask_busy() || reaped_gen == cmd_gen) return false;
    ata_wait_hook = usb_link_pump;
    reaped_gen = cmd_gen;
    if (rc) *rc = cmd_rc;
    return true;
}
