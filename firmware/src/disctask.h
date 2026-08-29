// disctask.h - the core1 producer, driven from core0. One job: a track's
// blocks into the PSRAM slots. The drive is core1's for the job's duration.
#ifndef DISCTASK_H
#define DISCTASK_H

#include <stdbool.h>
#include <stdint.h>

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

// Arm the track producer at a block. Returns once core1 has been signaled.
int  disctask_track(uint32_t start_seq);

// Ask the producer to wind up and wait for it, pumping USB meanwhile.
int  disctask_stop(uint32_t timeout_ms);

bool disctask_busy(void);

// A job that ended on its own leaves the drive's waits unhooked: from the idle
// loop, true once per such job, with its result.
bool disctask_reap(int *rc);

#endif
