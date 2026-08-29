// host_loop.c - bgmloop.h against a brute-force unrolling of the loop:
//   gcc -O2 -I src -o /tmp/host_loop test/host_loop.c && /tmp/host_loop

#include <stdint.h>
#include <stdio.h>

#include "bgmloop.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// Playback sample i of a [ls, le) loop, as a source sample index.
static uint32_t src_of(uint32_t i, uint32_t ls, uint32_t le)
{
    return i < le ? i : ls + (i - le) % (le - ls);
}

// Walk visits through three laps and compare every sample to the model.
static void run(const char *name, uint32_t ls, uint32_t le, uint32_t spr,
                uint32_t nblocks)
{
    bgm_loop_t l;
    bgm_loop_init(&l, ls, le, spr, nblocks);
    if (!le) { ls = 0; le = nblocks * spr; }
    uint32_t total = le + 3u * (le - ls);
    uint32_t i = 0, seq = 0, bad = 0;
    while (i < total) {
        uint32_t blk = bgm_loop_block(&l, seq);
        uint32_t end = bgm_loop_end_off(&l, seq, spr);
        uint32_t laps = i < le ? 0 : 1u + (i - le) / (le - ls);
        if (end == 0 || end > spr || blk >= nblocks) { bad++; break; }
        if (bgm_loop_laps(&l, seq) != laps) bad++;
        for (uint32_t off = 0; off < end && i < total; off++, i++)
            if (blk * spr + off != src_of(i, ls, le)) bad++;
        seq++;
    }
    char what[96];
    snprintf(what, sizeof(what), "%s: %lu visits, %lu samples", name,
             (unsigned long)seq, (unsigned long)i);
    check(bad == 0, what);
}

static void whole(uint32_t spr, uint32_t nblocks)
{
    bgm_loop_t l;
    bgm_loop_init(&l, 0, 0, spr, nblocks);
    uint32_t bad = 0;
    for (uint32_t seq = 0; seq < 3u * nblocks; seq++) {
        if (bgm_loop_block(&l, seq) != seq % nblocks) bad++;
        if (bgm_loop_end_off(&l, seq, spr) != spr) bad++;
        if (bgm_loop_laps(&l, seq) != seq / nblocks) bad++;
    }
    char what[96];
    snprintf(what, sizeof(what), "whole array (%lu blocks of %lu) == seq %% nblocks",
             (unsigned long)nblocks, (unsigned long)spr);
    check(bad == 0, what);
}

int main(void)
{
    whole(1470, 40);
    whole(4, 1);
    run("whole array as a range", 0, 0, 1470, 40);
    run("mid-block end", 2 * 1470, 37 * 1470 + 250, 1470, 38);
    run("mid-block end, dead blocks after", 2 * 1470, 37 * 1470 + 250, 1470, 40);
    run("block-boundary end", 3 * 1470, 20 * 1470, 1470, 20);
    run("start at 0", 0, 10 * 1470 + 7, 1470, 11);
    run("loop inside one block", 5 * 1470, 5 * 1470 + 300, 1470, 6);
    run("one-sample loop", 5 * 1470, 5 * 1470 + 1, 1470, 6);
    run("tiny blocks", 8, 37, 4, 10);
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
