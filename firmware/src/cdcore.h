// cdcore.h - the pieces every module here shares: one error namespace, the
// container crc, and the two-run span a block is handed out as.
#ifndef CDCORE_H
#define CDCORE_H

#include <stddef.h>
#include <stdint.h>

#define CD_OK          0
// -1..-7 are EDN8_*, returned by the cart link and passed straight through.
#define CD_EDISCIO   (-20)          // the drive failed a read we cannot skip
#define CD_EMEDIUM   (-21)          // the tray opened mid-read: the disc is gone
#define CD_EPSRAM    (-22)          // no psram: nowhere to buffer
#define CD_ESLOTS    (-23)          // the slot array cannot hold this block width
#define CD_EVERSION  (-24)
#define CD_ECRC      (-25)
#define CD_ESHAPE    (-26)          // counts or offsets outside the image
#define CD_ENOAUDIO  (-27)
#define CD_ENOGAME   (-28)          // no NESCDISC catalog at LBA 0
#define CD_EGAMEFMT  (-29)          // the catalog is inconsistent
#define CD_ENOITEM   (-30)          // no item of that type and id
#define CD_EROMBIG   (-31)          // the rom item does not fit the staging window
#define CD_ETRKFMT   (-32)          // the track item header is inconsistent
#define CD_ENOIMAGE  (-33)          // no N8PUSH1 magic
#define CD_EMENU     (-34)          // the cart is not on its ROM-select menu
#define CD_EMBOX     (-35)          // the mailbox read caught a write in flight

const char *cd_strerror(int rc);

// zlib crc32, nibble table. Step a chunk at a time from 0xFFFFFFFF and invert
// the result; cd_crc32() is the whole-buffer wrapper.
uint32_t cd_crc32_step(uint32_t c, const uint8_t *p, size_t n);
uint32_t cd_crc32(const uint8_t *p, size_t n);

// A block reaches the DAC as at most two contiguous runs, since a slot read may
// wrap the region it lives in.
typedef struct {
    const uint8_t *a; uint32_t na;
    const uint8_t *b; uint32_t nb;
} cd_span_t;

#endif
