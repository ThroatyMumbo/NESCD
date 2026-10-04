#!/usr/bin/env python3
"""The CD player ROM on pyntendo, with this script standing in for the host.

Boots build/cdplayer.nes on a plain NROM cart with CHR RAM, pokes the host's
half of the mailbox page straight into CHR, presses the pad, and reads the
screen back out of the nametable: the magic, D-pad focus and every button's
request byte, the echo / refusal / give-up ends of a request, the seek repeat,
the sequence wrapping, the tiles the status bytes render as, the spectrum
bars' sprites, and the NMI's cycle count against vblank.

    python3 emu_smoketest.py          # needs `pip install pyntendo`
"""
import os
import re
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
GFX = {k: int(v, 16) for k, v in re.findall(r"^(\w+) = \$([0-9A-F]+)$",
                                            open(os.path.join(HERE, "build", "gfx.inc")).read(), re.M)}
SYM = {k: int(v, 16) for v, k in re.findall(r"^al ([0-9A-F]+) \.(\w+)$",
                                            open(os.path.join(HERE, "build", "cdplayer.sym")).read(), re.M)}

PAGE = 0x1FF0
VIZ = 0x1FE8
OAM = 0x0200
VBLANK_CYCLES = 2273        # NTSC: 20 lines of 341 dots, 3 dots a CPU cycle
VIZ_DECAY, PEAK_HOLD = 6, 30
REQ, PARAM, ANS, STATE, TRACK, NTRACK, MIN, SEC = 8, 9, 10, 11, 12, 13, 14, 15
LMIN, LSEC = 4, 5
Z_BUSY = 0x17
Z_WAITLO = 0x18             # cdplayer.s: the give-up countdown
Z_FLASH = 0x1A
Z_SEL = 0x2A

PLAY, PAUSE, STOP, NEXT, PREV, EJECT, LOAD, SEEK = range(1, 9)
B_PREV, B_REW, B_PLAY, B_STOP, B_FF, B_NEXT, B_EJECT = range(7)

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
    def __init__(self, rom_path, screen=None):
        rom = ROM(rom_path, verbose=False)
        self.cart = NESCart0(prg_rom_data=rom.prg_rom_data, chr_rom_data=rom.chr_rom_data,
                             nametable_mirror_pattern=rom.mirror_pattern)
        self.il = Listener()
        self.ppu = NESPPU(cart=self.cart, screen=screen or NoScreen(), interrupt_listener=self.il)
        self.pad = ControllerBase(active=True)
        self.memory = NESMappedRAM(ppu=self.ppu, apu=None, cart=self.cart,
                                   controller1=self.pad,
                                   controller2=ControllerBase(active=False),
                                   interrupt_listener=self.il)
        self.cpu = MOS6502(memory=self.memory, undocumented_support_level=2,
                           stack_underflow_causes_exception=False)
        self.cpu.reset()
        self.seq = 0
        self.nmi_rti = SYM["irq"] - 1       # the nmi's rti sits just before irq
        self.nmi_cyc = None
        self.nmi_worst = 0

    def frame(self):
        guard = 0
        while True:
            if self.il.nmi_active():
                c = self.cpu.trigger_nmi()
                self.il.reset_nmi()
                self.nmi_cyc = 0
            elif self.il.oam_dma_pause():
                c = self.cpu.oam_dma_pause()
                self.il.reset_oam_dma_pause()
            else:
                pc = self.cpu.PC
                c = self.cpu.run_next_instr()
                if pc == self.nmi_rti and self.nmi_cyc is not None:
                    self.nmi_worst = max(self.nmi_worst, self.nmi_cyc + c)
                    self.nmi_cyc = None
            if self.nmi_cyc is not None:
                self.nmi_cyc += c
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
        self.frames(6)

    def bands(self, *v):
        self.cart.chr_mem[VIZ:VIZ + len(v)] = bytes(v)

    def meter(self, band):
        """Band's segments bottom first: '.' unlit, '#' lit, '^' peak."""
        n = GFX["VIZ_BANDS"]
        segs = ""
        for row in reversed(range(GFX["VIZ_ROWS"])):
            t = self.memory.ram[OAM + (row * n + band) * 4 + 1]
            segs += ".#^"[t % 3] + ".#^"[t // 3]
        return segs

    def press(self, button, hold=2):
        st = [0] * 8
        st[button] = 1
        self.pad.set_state(st)
        self.frames(hold)
        self.pad.set_state([0] * 8)
        self.frames(2)

    def focus(self, btn):
        while self.zp(Z_SEL) != btn:
            self.press(RIGHT)

    def next_req(self, cmd):
        self.seq = self.seq % 7 + 1
        return self.seq << 4 | cmd

    def answer(self):
        self.poke(ANS, self.page(REQ))
        self.frames(3)

    def tiles(self, addr, n):
        return [self.ppu.vram.read(addr + i) for i in range(n)]

    def text(self, addr, n):
        return "".join(chr(t) for t in self.tiles(addr, n))

    def pix(self, t):
        return bytes(self.cart.chr_mem[t * 16:t * 16 + 16])

    def big(self, addr):
        """By pixels: the static screen may name a blank half by an identical tile."""
        top, bot = self.tiles(addr, 2), self.tiles(addr + 32, 2)
        out = ""
        for t, b in zip(top, bot):
            ds = [d for d in range(11) if self.pix(GFX["T_BIGT"] + d) == self.pix(t)
                  and self.pix(GFX["T_BIGB"] + d) == self.pix(b)]
            if len(ds) != 1:
                return "??"
            out += "-" if ds[0] == 10 else str(ds[0])
        return out

    def bar(self):
        return [t - GFX["T_BAR"] for t in self.tiles(GFX["BAR_AT"], GFX["BAR_TILES"])]

    def mark(self):
        at = [i for i in range(GFX["NBTN"])
              if self.tiles(GFX["MARK_AT"] + i * GFX["BTN_PITCH"], 2)
              == [GFX["T_MARK_L"], GFX["T_MARK_R"]]]
        return at[0] if len(at) == 1 else at

    def icon(self):
        at = GFX["ICON_AT"]
        return self.tiles(at, 2) + self.tiles(at + 32, 2)

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


def word(mark, s):
    return chr(mark) + " " + s.ljust(9)


def main():
    s = Sys(ROM_PATH)
    s.frames(12)                # two vblank waits, the CHR upload, the static screen
    T_PLAY, T_PAUSE = GFX["T_PLAY"], GFX["T_PAUSE"]
    check(bytes(s.cart.chr_mem[PAGE:PAGE + 4]) == b"NCDP", "magic NCDP at $1FF0 every frame")
    check(s.page(REQ) == 0 and s.page(PARAM) == 0, "no request at boot")
    check(s.text(GFX["STATE_AT"], 11) == word(0x15, "NO DISC"), "screen: NO DISC before the host says anything")
    check(s.big(GFX["TRACK_AT"]) == "--" and s.big(GFX["MIN_AT"]) == "--"
          and s.text(GFX["LMIN_AT"], 2) == "--", "screen: blank numbers with no disc")
    check(s.mark() == 0, "screen: focus mark under the first button")
    check(s.icon() == [T_PLAY + i for i in range(4)], "screen: play icon while stopped")
    check(s.bar() == [0] * 20, "screen: empty progress bar")

    nb, nr = GFX["VIZ_BANDS"], GFX["VIZ_ROWS"]
    oam = s.memory.ram[OAM:OAM + 256]
    check(all(oam[(r * nb + b) * 4] == GFX["VIZ_Y"] - 1 + 8 * r
              and oam[(r * nb + b) * 4 + 3] == GFX["VIZ_X"] + GFX["VIZ_PITCH"] * b
              for r in range(nr) for b in range(nb))
          and all(oam[i] == 0xFF for i in range(nr * nb * 4, 256, 4)),
          "viz: bar sprites on the window's grid, the rest off screen")
    check(s.ppu.ppu_mask & 0x10, "viz: sprites enabled")
    check(bytes(s.cart.chr_mem[VIZ:VIZ + 8]) == bytes(8), "viz: bands clear at boot")
    check(all(s.meter(b) == "." * 12 for b in range(nb)), "viz: every segment unlit at boot")
    s.bands(255, 128, 0, 0, 0, 10)
    s.frames(2)
    check(s.meter(0) == "#" * 12 and s.meter(1) == "#" * 6 + "." * 6
          and s.meter(2) == "." * 12 and s.meter(5) == "." * 12, "viz: levels 255 / 128 / 0 / 10")
    s.frames(60)
    check(s.meter(1) == "#" * 6 + "." * 6, "viz: a held level holds, its peak inside the bar")
    s.bands(0, 0, 0, 0, 0, 0)
    s.frames(10)
    m = s.meter(0)
    check(8 <= m.count("#") <= 10 and m[11] == "^" and m.rstrip("^").endswith("."),
          f"viz: the bar falls, the peak hangs at the top ({m})")
    s.frames(40)
    m = s.meter(0)
    check(m.count("#") == 0 and "^" in m and m.index("^") < 11,
          f"viz: the bar is down, the peak falls after its hold ({m})")
    s.frames(60)
    check(all(s.meter(b) == "." * 12 for b in range(nb)), "viz: silence leaves every segment unlit")

    s.status(3, 5, 12, 1, 23, 4, 56)
    check(s.text(GFX["STATE_AT"], 11) == word(0x12, "STOPPED"), "screen: state word follows $1FFB")
    check(s.big(GFX["TRACK_AT"]) == "05" and s.text(GFX["NTRACK_AT"], 2) == "12", "screen: track / count")
    check(s.big(GFX["MIN_AT"]) == "01" and s.big(GFX["SEC_AT"]) == "23"
          and s.text(GFX["LMIN_AT"], 2) == "04" and s.text(GFX["LSEC_AT"], 2) == "56",
          "screen: elapsed / length")
    check(s.bar() == [8] * 5 + [4] + [0] * 14, "screen: bar at 83 of 296 s is 44 px")

    for b in (B, START, SELECT, UP, DOWN):
        s.press(b)
    check(s.page(REQ) == 0, "B, Start, Select, Up, Down send nothing")
    s.press(RIGHT)
    check(s.zp(Z_SEL) == 1 and s.mark() == 1, "Right moves the focus and its mark")
    s.press(LEFT)
    s.press(LEFT)
    check(s.zp(Z_SEL) == 6 and s.mark() == 6, "Left wraps from the first button to the last")
    s.press(RIGHT)
    check(s.zp(Z_SEL) == 0 and s.mark() == 0, "Right wraps from the last to the first")
    check(s.page(REQ) == 0, "moving the focus sends nothing")

    s.focus(B_PLAY)
    s.press(A)
    check(s.page(REQ) == s.next_req(PLAY) and s.page(PARAM) == 0, "A on play while stopped -> PLAY, track 0")
    check(s.zp(Z_BUSY) == 1, "the request is outstanding")
    s.press(RIGHT)
    s.press(A)
    check(s.page(REQ) == (s.seq << 4 | PLAY), "a press while busy is ignored")
    s.press(LEFT)
    s.answer()
    check(s.zp(Z_BUSY) == 0 and s.mark() == B_PLAY, "the echo ends the request, the mark stays lit")

    s.status(4, 5, 12, 1, 23, 4, 56)
    check(s.icon() == [T_PAUSE + i for i in range(4)], "screen: pause icon while playing")
    s.press(A)
    check(s.page(REQ) == s.next_req(PAUSE), "A on play while playing -> PAUSE")
    s.poke(ANS, s.page(REQ) | 0x80)
    s.frames(4)
    check(s.zp(Z_FLASH) > 0 and s.ppu.vram.read(GFX["PAL_FACE"]) == GFX["FACE_RED"],
          "a refusal turns the button faces red")
    check(s.zp(Z_BUSY) == 0, "the refusal ends the request")
    s.frames(50)
    check(s.ppu.vram.read(GFX["PAL_FACE"]) == GFX["FACE_NORMAL"], "the red times out")

    s.status(5, 5, 12, 1, 23, 4, 56)
    check(s.icon() == [T_PLAY + i for i in range(4)], "screen: play icon while paused")
    s.press(A)
    check(s.page(REQ) == s.next_req(PAUSE), "A on play while paused -> PAUSE (resume)")
    s.answer()

    s.focus(B_FF)
    s.press(A)
    check(s.page(REQ) == s.next_req(SEEK) and s.page(PARAM) == 10, "A on >> -> SEEK +10")
    s.answer()
    s.focus(B_REW)
    s.press(A)
    check(s.page(REQ) == s.next_req(SEEK) and s.page(PARAM) == 0xF6, "A on << -> SEEK -10")
    s.answer()

    s.pad.set_state([1, 0, 0, 0, 0, 0, 0, 0])
    s.frames(2)
    first = s.next_req(SEEK)
    check(s.page(REQ) == first, "holding A on <<: the first seek")
    s.frames(40)
    check(s.page(REQ) == first, "held, unanswered: no second seek")
    s.answer()
    s.frames(30)
    check(s.page(REQ) == s.next_req(SEEK), "held and answered: it repeats")
    s.pad.set_state([0] * 8)
    s.frames(2)
    s.answer()
    s.frames(40)
    check(s.page(REQ) == (s.seq << 4 | SEEK), "released: the repeat stops")

    s.focus(B_STOP)
    s.press(A)
    check(s.page(REQ) == s.next_req(STOP), "A on stop -> STOP")
    s.answer()
    s.focus(B_NEXT)
    s.press(A)
    check(s.page(REQ) == s.next_req(NEXT) and s.page(PARAM) == 0, "A on >>| -> NEXT")
    s.answer()
    s.focus(B_PREV)
    s.press(A)
    check(s.page(REQ) == s.next_req(PREV), "A on |<< -> PREV")
    s.set_zp(Z_WAITLO, 6)
    s.set_zp(Z_WAITLO + 1, 0)
    s.frames(10)
    check(s.zp(Z_BUSY) == 0, "an unanswered request gives up")

    s.focus(B_EJECT)
    s.press(A)
    check(s.page(REQ) == s.next_req(EJECT), "A on eject with a disc in -> EJECT")
    s.answer()
    s.status(1, 0, 0, 0, 0)
    check(s.text(GFX["STATE_AT"], 11) == word(0x13, "TRAY OPEN"), "screen: tray open")
    check(s.big(GFX["TRACK_AT"]) == "--" and s.bar() == [0] * 20, "screen: numbers blank, bar empty")
    s.press(A)
    check(s.page(REQ) == s.next_req(LOAD), "A on eject with the tray open -> LOAD")
    s.answer()

    while s.seq != 7:
        s.press(A)
        s.next_req(LOAD)
        s.answer()
    s.press(A)
    check(s.page(REQ) == s.next_req(LOAD) and s.seq == 1, "sequence wraps to 1, bit 7 never set")
    s.answer()

    s.status(4, 12, 12, 59, 59, 61, 5)
    check(s.text(GFX["STATE_AT"], 11) == word(0x10, "PLAYING")
          and s.big(GFX["MIN_AT"]) + s.big(GFX["SEC_AT"]) == "5959"
          and s.text(GFX["LMIN_AT"], 2) + s.text(GFX["LSEC_AT"], 2) == "6105",
          "screen: two-digit fields")
    check(s.bar() == [8] * 19 + [5], "screen: bar at 3599 of 3665 s is 157 px")
    s.status(4, 1, 1, 9, 0, 3, 0)
    check(s.bar() == [8] * 20, "screen: elapsed past the length fills the bar")
    s.status(9, 1, 1, 0, 0)
    check(s.text(GFX["STATE_AT"], 11) == word(0x15, "NO DISC"), "screen: an unknown state reads as NO DISC")
    check(s.nmi_worst < VBLANK_CYCLES, f"nmi fits vblank: worst {s.nmi_worst} of {VBLANK_CYCLES} cycles")

    print(f"\n{fails} FAILURE(S)" if fails else "\nall checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
