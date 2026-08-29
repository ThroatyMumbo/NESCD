// n8push.h - push staged files onto the EverDrive's SD card and boot one.
//
// The image is staged in PSRAM off the disc's ROM item, or in QSPI flash over
// SWD; this reads it in place and replays the EverDrive's own file protocol
// over the cart link (tools/mkn8push.py builds one). The
// cart must be sitting on its ROM-select menu: it leaves the menu the moment a
// game boots, so every file and the install have to go in one pass.
#ifndef N8PUSH_H
#define N8PUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdcore.h"

// 1 MiB into the 16 MiB QSPI flash, clear of the firmware below (~116 KiB).
#define N8PUSH_BASE  0x10100000u
#define N8PUSH_LIMIT (3u << 20)

#define N8PUSH_MAGIC   "N8PUSH1"
#define N8PUSH_VERSION 1u
#define N8PUSH_MAXFILE 8u
#define N8PUSH_PATH    64u

// Little-endian and naturally aligned: the image is used in place, so this is a
// cast, not a parse.
typedef struct {
    uint32_t off, len;
    char     path[N8PUSH_PATH];     // destination on the cart's SD, NUL-padded
} n8push_file_t;

typedef struct {
    char     magic[8];              // "N8PUSH1\0"
    uint32_t version;
    uint32_t nfiles;
    uint32_t payload_off;
    uint32_t payload_bytes;
    uint32_t crc32;                 // over the payload region
    uint32_t reserved[2];
    char     boot[N8PUSH_PATH];     // menu_install target; "" pushes without booting
    n8push_file_t file[N8PUSH_MAXFILE];
} n8push_hdr_t;

// Validate the staged image. Valid only until something else is staged there.
int n8push_open(bool check_crc);

// The same against an arbitrary mapping, which is how test/host_push.c runs the
// real parser over a packed image on the PC.
int n8push_open_at(const void *base, size_t limit, bool check_crc);
const n8push_hdr_t *n8push_hdr(void);
const uint8_t *n8push_data(const n8push_file_t *f);

// Push every staged file, then install and start hdr->boot if it is set.
// Prints its own progress; needs an open cart link.
int n8push_run(void);

#endif
