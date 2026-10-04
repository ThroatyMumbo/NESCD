// host_viz.c - src/viz.c off the bench. No argument: sines at each band's
// centre must light that band alone. With a file of s16le stereo 44.1 kHz
// (ffmpeg -f s16le -ac 2 -ar 44100), the band levels at 30 Hz, as percentiles.
//   gcc -O2 -I src -o /tmp/host_viz test/host_viz.c src/viz.c -lm

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viz.h"

static int fails;

static void sine_checks(void)
{
    static const float hz[VIZ_BANDS] = { 65, 150, 280, 560, 1100, 2200, 4700, 10000 };
    for (int b = 0; b < VIZ_BANDS; b++) {
        for (uint32_t i = 0; i < 4 * VIZ_N; i++) {
            int32_t s = (int32_t)(3277.0f * sinf(2.0f * 3.14159265f * hz[b] * (float)i / 44100.0f));
            viz_tap(s, s);
        }
        uint8_t v[VIZ_BANDS];
        viz_bands(v);
        int ok = v[b] > 120;
        for (int o = 0; o < VIZ_BANDS; o++)
            if (o != b && v[o] + 60 > v[b]) ok = 0;
        printf("  %5.0f Hz at -20 dBFS:", hz[b]);
        for (int o = 0; o < VIZ_BANDS; o++) printf(" %3u", v[o]);
        printf("  %s\n", ok ? "ok" : "FAIL");
        fails += !ok;
    }
    memset(viz_ring, 0, sizeof(viz_ring));
    uint8_t v[VIZ_BANDS];
    viz_bands(v);
    int ok = 1;
    for (int o = 0; o < VIZ_BANDS; o++) ok &= v[o] == 0;
    printf("  silence:%s\n", ok ? " ok" : " FAIL");
    fails += !ok;
}

static int cmp(const void *a, const void *b) { return *(const uint8_t *)a - *(const uint8_t *)b; }

static void file_levels(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t cap = 1 << 16, n = 0;
    uint8_t (*lv)[VIZ_BANDS] = malloc(cap * VIZ_BANDS);
    int16_t fr[2];
    uint32_t k = 0;
    while (fread(fr, sizeof(fr), 1, f) == 1) {
        viz_tap(fr[0], fr[1]);
        if (++k % 1470u) continue;
        if (n == cap) lv = realloc(lv, (cap *= 2) * VIZ_BANDS);
        viz_bands(lv[n++]);
    }
    fclose(f);
    printf("  %zu frames\n  band   p10 p50 p90 p99 (segments of 12 at p50/p90)\n", n);
    uint8_t *col = malloc(n ? n : 1);
    for (int b = 0; b < VIZ_BANDS; b++) {
        for (size_t i = 0; i < n; i++) col[i] = lv[i][b];
        qsort(col, n, 1, cmp);
        uint8_t p50 = col[n / 2], p90 = col[n * 9 / 10];
        printf("  %d     %3u %3u %3u %3u  %2d / %2d\n", b, col[n / 10], p50, p90, col[n * 99 / 100],
               (p50 * 12 + 128) >> 8, (p90 * 12 + 128) >> 8);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) { file_levels(argv[1]); return 0; }
    sine_checks();
    printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
    return fails != 0;
}
