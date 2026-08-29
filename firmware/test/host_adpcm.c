// host_adpcm.c - decode a track's audio with the FIRMWARE's decoder.
//
// Links src/ima.h, so this is the exact code the DMA IRQ runs. Writes raw
// s16le to stdout; tools/mktrack.py --verify decodes the same image in Python,
// which is the only way to know the two ends of the format agree.
//
//   gcc -O2 -I src -o /tmp/host_adpcm test/host_adpcm.c
//   /tmp/host_adpcm track.rom > /tmp/c.raw
//
// Blocks are walked by the array indexing track.c uses, so this exercises the
// container arithmetic and not just the codec.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ima.h"

#define AFMT_PCM16  1u
#define AFMT_ADPCM4 2u

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void emit_block(const uint8_t *b, uint32_t fmt, uint32_t spr, FILE *out)
{
    if (fmt == AFMT_PCM16) {
        fwrite(b, 2, spr, out);
        return;
    }
    ima_state_t s;
    s.pred = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    s.index = b[2];
    if (s.index > 88) { fprintf(stderr, "step_index %u\n", s.index); exit(1); }
    if (b[3] != 0) { fprintf(stderr, "reserved byte not zero\n"); exit(1); }
    for (uint32_t i = 0; i < spr; i++) {
        uint8_t byte = b[IMA_HDR_LEN + (i >> 1)];
        uint8_t nib = (i & 1u) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
        int16_t v = (int16_t)ima_step(&s, nib);
        fputc(v & 0xFF, out);
        fputc((v >> 8) & 0xFF, out);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: host_adpcm IMAGE\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)n);
    if (fread(img, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);

    uint32_t fmt, spr, blk, nblocks = 0;

    if (memcmp(img, "CDTRACK\0", 8)) {
        fprintf(stderr, "not a CDTRACK track image\n");
        return 1;
    }
    uint32_t nframes = rd32(img + 12);
    fmt = rd32(img + 52); spr = rd32(img + 64); blk = rd32(img + 68);
    uint32_t aoff = rd32(img + 72), abytes = rd32(img + 76);
    if (!fmt) { fprintf(stderr, "no audio\n"); return 1; }
    if (abytes != nframes * blk) {
        fprintf(stderr, "audio_bytes %u != %u blocks * %u\n", abytes, nframes, blk);
        return 1;
    }
    for (uint32_t i = 0; i < nframes; i++)
        emit_block(img + aoff + (size_t)i * blk, fmt, spr, stdout);
    nblocks = nframes;

    fprintf(stderr, "  %u blocks, fmt %u, %u spr, %u B/block -> %u samples\n",
            nblocks, fmt, spr, blk, nblocks * spr);
    return 0;
}
