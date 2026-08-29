// bgmloop.h - a track's loop range folded onto a monotonic block cursor.
// Pico-free, shared by the players and test/host_loop.c.

#ifndef BGMLOOP_H
#define BGMLOOP_H

#include <stdint.h>

// Visits 0..le_blk play the array from the top; every later visit lands in
// ls_blk..le_blk, and a lap's last visit stops after le_off samples.
typedef struct { uint32_t ls_blk, le_blk, le_off, lap; } bgm_loop_t;

// loop_start is block-aligned (the masterer leads the source in for it);
// 0/0 loops the whole array, which reduces the fold to seq % nblocks.
static inline void bgm_loop_init(bgm_loop_t *l, uint32_t loop_start,
                                 uint32_t loop_end, uint32_t spr, uint32_t nblocks)
{
    if (!loop_end) { loop_start = 0; loop_end = nblocks * spr; }
    l->ls_blk = loop_start / spr;
    l->le_blk = (loop_end - 1u) / spr;
    l->le_off = loop_end - l->le_blk * spr;
    l->lap = l->le_blk - l->ls_blk + 1u;
}

static inline __attribute__((always_inline)) uint32_t
bgm_loop_block(const bgm_loop_t *l, uint32_t seq)
{
    return seq <= l->le_blk ? seq : l->ls_blk + (seq - l->le_blk - 1u) % l->lap;
}

// Samples this visit plays: the range's tail on a lap's last block, else all.
static inline __attribute__((always_inline)) uint32_t
bgm_loop_end_off(const bgm_loop_t *l, uint32_t seq, uint32_t spr)
{
    return bgm_loop_block(l, seq) == l->le_blk ? l->le_off : spr;
}

static inline uint32_t bgm_loop_laps(const bgm_loop_t *l, uint32_t seq)
{
    return seq <= l->le_blk ? 0 : 1u + (seq - l->le_blk - 1u) / l->lap;
}

#endif
