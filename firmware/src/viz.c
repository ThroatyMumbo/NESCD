// viz.c - see viz.h.

#include <math.h>
#include <stdbool.h>

#include "viz.h"

#define LOG2N    10
#define RANGE_DB 36.0f              // full bar to empty, below the gain's top
#define TILT_DB  3.0f                // per band: music falls off with frequency
#define TOP_MIN  (-24.0f)            // the gain's ceiling on quiet material
#define TOP_FALL 0.03f               // dB per call, ~1 dB/s at 30 Hz

int16_t viz_ring[VIZ_N];
volatile uint32_t viz_w;

// Bin edges at 43.07 Hz a bin: 43-130, -215, -390, -730, -1.5k, -3k, -6.5k, -16k Hz.
static const uint16_t edge[VIZ_BANDS + 1] = { 1, 3, 5, 9, 17, 34, 70, 150, 372 };

static float hann[VIZ_N], cosv[VIZ_N / 2], sinv[VIZ_N / 2];
static float re[VIZ_N], im[VIZ_N];
static float ref_pow;
static float top = TOP_MIN;          // the loudest band lately, in dB: full scale
static bool ready;

static void init(void)
{
    const float pi = 3.14159265f;
    for (uint32_t i = 0; i < VIZ_N; i++)
        hann[i] = 0.5f - 0.5f * cosf(2.0f * pi * (float)i / (float)VIZ_N);
    for (uint32_t i = 0; i < VIZ_N / 2; i++) {
        cosv[i] = cosf(2.0f * pi * (float)i / (float)VIZ_N);
        sinv[i] = -sinf(2.0f * pi * (float)i / (float)VIZ_N);
    }
    // A full-scale sine through the Hann window: peak bin A*N/4, 1.5x that over its lobe.
    float pk = 32767.0f * (float)VIZ_N / 4.0f;
    ref_pow = 1.5f * pk * pk;
    ready = true;
}

static void fft(void)
{
    for (uint32_t i = 1, j = 0; i < VIZ_N; i++) {
        uint32_t bit = VIZ_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2, step = VIZ_N / 2; len <= VIZ_N; len <<= 1, step >>= 1) {
        uint32_t half = len >> 1;
        for (uint32_t i = 0; i < VIZ_N; i += len) {
            for (uint32_t k = 0; k < half; k++) {
                float wr = cosv[k * step], wi = sinv[k * step];
                uint32_t a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

void viz_bands(uint8_t out[VIZ_BANDS])
{
    if (!ready) init();
    uint32_t w = viz_w;
    for (uint32_t i = 0; i < VIZ_N; i++) {
        re[i] = (float)viz_ring[(w + i) & (VIZ_N - 1u)] * hann[i];
        im[i] = 0.0f;
    }
    fft();
    float db[VIZ_BANDS], loud = -120.0f;
    for (int b = 0; b < VIZ_BANDS; b++) {
        float p = 0.0f;
        for (uint32_t k = edge[b]; k < edge[b + 1]; k++) p += re[k] * re[k] + im[k] * im[k];
        db[b] = 10.0f * log10f(p / ref_pow + 1e-12f) + TILT_DB * (float)b;
        if (db[b] > loud) loud = db[b];
    }
    // Rises at once and falls slowly, so a hot master and a quiet one both fill the bars.
    top -= TOP_FALL;
    if (loud > top) top = loud;
    if (top < TOP_MIN) top = TOP_MIN;
    for (int b = 0; b < VIZ_BANDS; b++) {
        float v = (db[b] - top + RANGE_DB) * (255.0f / RANGE_DB);
        out[b] = v <= 0.0f ? 0u : v >= 255.0f ? 255u : (uint8_t)v;
    }
}
