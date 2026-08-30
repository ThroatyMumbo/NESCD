// mailbox.h - the game's mailbox in the cart's CHR RAM.
//
// $1FF8 is the game's request, $1FF9 the level-triggered desired-music byte,
// $1FFA the host's answer. CHR rather than SRM or PRG because a host poll of
// CHR only blips a PPU fetch, while a poll of SRM/PRG steals the CPU's bus.
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stddef.h>
#include <stdint.h>

// A stock core maps 8 KiB of CHR RAM at the upper 4 MB of the CHR window
// (base_sv/everdrive.sv: chr_addr_msk ORs bit 22 in), and forces that bit off
// for host DMA, so the host has to supply it. PPU $1FF8 is 0x401FF8 here.
#define MAILBOX_ADDR        0x401FF8u
#define MAILBOX_STATUS_ADDR (MAILBOX_ADDR + 2u)

// The answer byte: 0 idle, N once request N may proceed, $FF when nothing will.
// This build serves no requests, so $FF is the only answer it ever gives.
#define MAILBOX_FAIL 0xFFu

// The 16 bytes of sprite tile $FF, $1FF0..$1FFF, are the whole page; the CD
// player ROM uses the rest of it (player.h).
#define MAILBOX_PAGE        0x401FF0u
#define MAILBOX_HOST(ppu)   (0x400000u + (uint32_t)(ppu))

// The request pair, read twice because a poll can land mid-write; CD_EMBOX
// means the two disagreed.
int mailbox_rd(uint8_t v[2]);

// The same double read of n bytes at PPU address ppu ($1FF0..$1FFF).
int mailbox_rd_at(uint32_t ppu, uint8_t *v, size_t n);

int mailbox_status_wr(uint8_t v);
int mailbox_wr_at(uint32_t ppu, const uint8_t *v, size_t n);

#endif
