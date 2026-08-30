#!/usr/bin/env python3
"""The CD player ROM on pyntendo, with this script standing in for the host.

Boots build/cdplayer.nes on a plain NROM cart with CHR RAM, pokes the host's
half of the mailbox page straight into CHR, presses the pad, and reads the
screen back out of the nametable: the magic, every button's request byte,
the echo / refusal / give-up ends of a request, the sequence wrapping, and
the text the status bytes render as.

    python3 emu_smoketest.py          # needs `pip install pyntendo`
"""
import os
import sys

try:
    from nes.rom import ROM
    from nes.pycore.carts import NESCart0
    from nes.pycore.memory import NESMappedRAM
    from nes.pycore.mos6502 import MOS6502
    from nes.pycore.ppu import NESPPU
    from nes.peripherals import ControllerBase
except ImportError:
    print("emu_smoketest: pyntendo is not importable - skipped")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
ROM_PATH = os.path.join(HERE, "build", "cdplayer.nes")

PAGE = 0x1FF0
REQ, PARAM, ANS, STATE, TRACK, NTRACK, MIN, SEC = 8, 9, 10, 11, 12, 13, 14, 15
LMIN, LSEC = 4, 5
Z_WAITLO = 0x18             # cdplayer.s: the give-up countdown
Z_BUSY = 0x17

A, B, SELECT, START, UP, DOWN, LEFT, RIGHT = range(8)


class Listener:
    def __init__(self):
        self._nmi = self._irq = self._oam = False

    def nmi_active(self): return self._nmi
    def irq_active(self): return self._irq
    def oam_dma_pause(self): return self._oam
    def any_active(self): return self._nmi or self._irq or self._oam
    def raise_nmi(self): self._nmi = True
    def reset_nmi(self): self._nmi = False
    def raise_irq(self): self._irq = True
    def reset_irq(self): self._irq = False
    def raise_oam_dma_pause(self): self._oam = True
    def reset_oam_dma_pause(self): self._oam = False


class NoScreen:
    transparent_color = None

    def write_at(self, x, y, color):
        pass


class Sys:
    def __init__(self, rom_path):
        rom = ROM(rom_path, verbose=False)
        self.cart = NESCart0(prg_rom_data=rom.prg_rom_data, chr_rom_data=rom.chr_rom_data,
                             nametable_mirror_pattern=rom.mirror_pattern)
        self.il = Listener()
        self.ppu = NESPPU(cart=self.cart, screen=NoScreen(), interrupt_listener=self.il)
        self.pad = ControllerBase(active=True)
        self.memory = NESMappedRAM(ppu=self.ppu, apu=None, cart=self.cart,
                                   controller1=self.pad,
                                   controller2=ControllerBase(active=False),
                                   interrupt_listener=self.il)
        self.cpu = MOS6502(memory=self.memory, undocumented_support_level=2,
                           stack_underflow_causes_exception=False)
        self.cpu.reset()

    def frame(self):
        guard = 0
        while True:
            if self.il.nmi_active():
                c = self.cpu.trigger_nmi()
                self.il.reset_nmi()
            elif self.il.oam_dma_pause():
                c = self.cpu.oam_dma_pause()
                self.il.reset_oam_dma_pause()
            else:
                c = self.cpu.run_next_instr()
            if self.ppu.run_cycles(c * 3):
                return
            guard += 1
            if guard > 2_000_000:
                raise RuntimeError("no frame completed")

    def frames(self, n):
        for _ in range(n):
            self.frame()

    # -- the host's side of the page --
    def page(self, off):
        return self.cart.chr_mem[PAGE + off]

    def poke(self, off, v):
        self.cart.chr_mem[PAGE + off] = v

    def status(self, state, track, ntrack, mn, sc, lmn=0, lsc=0):
        for off, v in ((STATE, state), (TRACK, track), (NTRACK, ntrack), (MIN, mn),
                       (SEC, sc), (LMIN, lmn), (LSEC, lsc)):
            self.poke(off, v)

    def press(self, button, hold=2):
        st = [0] * 8
        st[button] = 1
        self.pad.set_state(st)
        self.frames(hold)
        self.pad.set_state([0] * 8)
        self.frames(2)

    def text(self, row, col, n):
        return "".join(chr(self.ppu.vram.read(0x2000 + row * 32 + col + i)) for i in range(n))

    def zp(self, addr):
        return self.memory.ram[addr]

    def set_zp(self, addr, v):
        self.memory.ram[addr] = v


fails = 0


def check(cond, what):
    global fails
    print(f"  {what:60s} {'ok' if cond else 'FAIL'}")
    if not cond:
        fails += 1


def main():
    s = Sys(ROM_PATH)
    s.frames(12)                # two vblank waits, the CHR upload, the static screen
    check(bytes(s.cart.chr_mem[PAGE:PAGE + 4]) == b"NCDP", "magic NCDP at $1FF0 every frame")
    check(s.page(REQ) == 0 and s.page(PARAM) == 0, "no request at boot")
    check(s.text(8, 13, 9) == "NO DISC  ", "screen: NO DISC before the host says anything")
    check(s.text(3, 9, 13) == "NES CD PLAYER", "screen: title")
    check(s.text(19, 9, 13) == "    A - PLAY " and s.text(21, 9, 13) == "    B - STOP "
          and s.text(23, 9, 13) == "START - PAUSE", "screen: the three button prompts, dashes aligned")

    s.status(3, 5, 12, 1, 23, 4, 56)
    s.frames(4)
    check(s.text(8, 13, 9) == "STOPPED  ", "screen: state word follows $1FFB")
    check(s.text(11, 13, 7) == "05 / 12", "screen: track / count")
    check(s.text(14, 13, 13) == "01:23 / 04:56", "screen: elapsed / length")

    s.press(A)
    check(s.page(REQ) == 0x11, "A -> PLAY, sequence 1")
    check(s.text(8, 23, 1) == "*", "screen: busy mark while the request is out")
    s.press(B)
    check(s.page(REQ) == 0x11, "a second press while busy is ignored")

    s.poke(ANS, 0x11)
    s.frames(4)
    check(s.text(8, 23, 1) == " ", "echo clears the busy mark")
    check(s.page(REQ) == 0x11, "the request byte keeps its value after the answer")

    s.press(RIGHT)
    check(s.page(REQ) == 0x24, "Right -> NEXT, sequence 2")
    s.poke(ANS, 0x24 | 0x80)
    s.frames(4)
    check(s.text(8, 23, 1) == "!", "refusal shows the mark")
    check(s.zp(Z_BUSY) == 0, "refusal ends the request")
    s.frames(50)
    check(s.text(8, 23, 1) == " ", "the refusal mark times out")

    s.press(LEFT)
    check(s.page(REQ) == 0x35, "Left -> PREV, sequence 3")
    s.set_zp(Z_WAITLO, 6)
    s.set_zp(Z_WAITLO + 1, 0)
    s.frames(10)
    check(s.zp(Z_BUSY) == 0 and s.text(8, 23, 1) == " ", "an unanswered request gives up")

    s.press(START)
    check(s.page(REQ) == 0x42, "Start -> PAUSE, sequence 4")
    s.poke(ANS, 0x42)
    s.frames(3)
    s.press(SELECT)
    check(s.page(REQ) == 0x56, "Select -> EJECT with a disc in")
    s.poke(ANS, 0x56)
    s.frames(3)

    s.status(1, 0, 0, 0, 0)
    s.frames(4)
    check(s.text(8, 13, 9) == "TRAY OPEN", "screen: tray open")
    s.press(SELECT)
    check(s.page(REQ) == 0x67, "Select -> LOAD with the tray open")
    s.poke(ANS, 0x67)
    s.frames(3)

    s.press(A)
    check(s.page(REQ) == 0x71, "sequence 7")
    s.poke(ANS, 0x71)
    s.frames(3)
    s.press(A)
    check(s.page(REQ) == 0x11, "sequence wraps to 1, bit 7 never set")
    s.poke(ANS, 0x11)
    s.frames(3)

    s.status(4, 12, 12, 59, 59, 61, 5)
    s.frames(4)
    check(s.text(8, 13, 9) == "PLAYING  " and s.text(14, 13, 13) == "59:59 / 61:05",
          "screen: two-digit fields")
    s.status(9, 1, 1, 0, 0)
    s.frames(4)
    check(s.text(8, 13, 9) == "NO DISC  ", "screen: an unknown state reads as NO DISC")

    print(f"\n{fails} FAILURE(S)" if fails else "\nall checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
