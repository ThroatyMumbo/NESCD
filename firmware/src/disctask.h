// disctask.h - the core1 producer, driven from core0. One job at a time, a
// track's or an audio disc's blocks into the PSRAM slots; the drive is core1's
// for the job's duration.
#ifndef DISCTASK_H
#define DISCTASK_H

#include <stdbool.h>
#include <stdint.h>

// A producer: run(arg) on core1 until it ends or stop() is seen, reset()
// before each run clears its flags. Pico-free, so the host harnesses link the
// producers without this module.
typedef struct {
    int  (*run)(uint32_t arg);
    void (*stop)(void);
    void (*reset)(void);
    const char *name;
} disc_job_t;

// The 8 MiB PSRAM part, split. The lower half is where the ROM item is staged
// before it goes to the cart; the upper half is the audio slots. Disjoint, but
// core1 must not own the drive while core0 stages off it.
#define ROM_STAGE_BYTES   (4u << 20)
#define DISC_SLOTS_OFF    (4u << 20)
#define DISC_SLOTS_BYTES  (4u << 20)

// Launch core1 into its parked command loop and point the slots at PSRAM.
// False if no PSRAM answered, in which case nothing else here may be called.
bool disctask_init(void);
bool disctask_ready(void);

// Arm a producer at a block. Returns once core1 has been signaled.
int  disctask_run(const disc_job_t *job, uint32_t start_seq);
int  disctask_track(uint32_t start_seq);   // run(&track_job, ...)

// The job last armed, NULL before the first; disctask_stop and the reap act
// on it.
const disc_job_t *disctask_job(void);

// Ask the producer to wind up and wait for it, pumping USB meanwhile.
int  disctask_stop(uint32_t timeout_ms);

bool disctask_busy(void);

// A job that ended on its own leaves the drive's waits unhooked: from the idle
// loop, true once per such job, with its result.
bool disctask_reap(int *rc);

#endif
