// mailbox.h - the game's music byte in the cart's CHR RAM.
//
// The game writes the track it wants there, level-triggered, and the host only
// ever reads it. CHR rather than SRM or PRG because a host poll of CHR only
// blips a PPU fetch, while a poll of SRM/PRG steals the CPU's bus.
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stddef.h>
#include <stdint.h>

// A stock core maps 8 KiB of CHR RAM at the upper 4 MB of the CHR window
// (base_sv/everdrive.sv: chr_addr_msk ORs bit 22 in), and forces that bit off
// for host DMA, so the host has to supply it. PPU $1FF9 is 0x401FF9 here.
#define MAILBOX_PPU         0x1FF9u

// The 16 bytes of sprite tile $FF, $1FF0..$1FFF, are the whole page; the CD
// player ROM uses the rest of it, and the end of tile $FE too (player.h).
#define MAILBOX_PAGE        0x401FF0u
#define MAILBOX_HOST(ppu)   (0x400000u + (uint32_t)(ppu))

int mailbox_rd(uint8_t *v);

void mailbox_at(uint32_t ppu);
uint32_t mailbox_where(void);

// A double read of n bytes at PPU address ppu; CD_EMBOX means the two disagreed.
int mailbox_rd_at(uint32_t ppu, uint8_t *v, size_t n);

int mailbox_wr_at(uint32_t ppu, const uint8_t *v, size_t n);

#endif
