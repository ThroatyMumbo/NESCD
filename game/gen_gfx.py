#!/usr/bin/env python3
# The CD player's screen: draws one 256x240 picture, cuts it into tiles against
# the fixed tiles the ROM writes at run time, and emits the CHR, the nametable
# with its attributes, the palette and the layout constants cdplayer.s includes.
import os
import sys

from gen_font import GLYPHS, MARKS
from gen_tiles import encode

W, H = 256, 240
P_PANEL, P_LCD, P_BTN = range(3)

PALETTE = [
    0x0F, 0x01, 0x21, 0x30,     # panel: black, navy, light blue, white
    0x0F, 0x0C, 0x1C, 0x2C,     # display: black, teal, dim segment, lit cyan
    0x0F, 0x10, 0x00, 0x30,     # buttons: black, face, shade, highlight
    0x0F, 0x0F, 0x0F, 0x0F,
    0x0F, 0x2A, 0x30, 0x0A,     # meter sprites: -, lit, peak, unlit; green
    0x0F, 0x28, 0x30, 0x08,     # yellow
    0x0F, 0x27, 0x30, 0x07,     # orange
    0x0F, 0x16, 0x30, 0x06,     # red
]
FACE_RED = 0x16

# The spectrum: VIZ_BANDS bars of VIZ_ROWS sprites, two LED segments a sprite.
# 8 bars is also the sprites-per-scanline limit; 6 px lit, 1 px apart.
VIZ_BANDS, VIZ_ROWS = 8, 6
VIZ_X, VIZ_Y, VIZ_PITCH = 16, 64, 7
WIN_X0, WIN_X1 = VIZ_X - 1, VIZ_X + VIZ_BANDS * VIZ_PITCH + 1   # the border columns
VIZ_PAL = [3, 2, 1, 1, 0, 0]          # sprite palette per row, top first

NBTN = 7
BTN_ROW, BTN_COL, BTN_PITCH = 20, 2, 4
BTN_PLAY = 2
MARK_ROW = 24
BAR_ROW, BAR_COL, BAR_TILES = 16, 6, 20

# Fixed tile ids: the ones the ROM picks at run time.
T_MARKS = 0x10
T_BIGT, T_BIGB = 0x60, 0x6B          # 7-segment digits 0..9 and blank
T_BAR = 0x76                         # 0..8 pixels filled
T_MARK_L, T_MARK_R = 0x7F, 0x80
T_PLAY, T_PAUSE = 0x81, 0x85         # 2x2 button icons, TL TR BL BR


def nt(row, col):
    return 0x2000 + row * 32 + col


TRACK_AT = nt(10, 11)
NTRACK_AT = nt(11, 14)
MIN_AT = nt(10, 17)
SEC_AT = nt(10, 20)
LMIN_AT = nt(11, 23)
LSEC_AT = nt(11, 26)
STATE_AT = nt(12, 11)
BAR_AT = nt(BAR_ROW, BAR_COL)
MARK_AT = nt(MARK_ROW, BTN_COL + 1)
ICON_AT = nt(BTN_ROW + 1, BTN_COL + BTN_PITCH * BTN_PLAY + 1)

canvas = [[0] * W for _ in range(H)]
attr = [[P_PANEL] * 16 for _ in range(15)]


def px(x, y, c):
    canvas[y][x] = c


def fill(x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            canvas[y][x] = c


def blit(x0, y0, pix):
    for y, row in enumerate(pix):
        for x, c in enumerate(row):
            canvas[y0 + y][x0 + x] = c


def pal(bx0, by0, bx1, by1, p):
    for by in range(by0, by1):
        for bx in range(bx0, bx1):
            attr[by][bx] = p


def cut(pix, tx, ty):
    return [row[tx * 8:tx * 8 + 8] for row in pix[ty * 8:ty * 8 + 8]]


# -- the fixed tiles ------------------------------------------------------------

def glyph(art):
    p = [[1] * 8 for _ in range(8)]
    ink = {(x, y) for y, r in enumerate(art) for x, ch in enumerate(r) if ch == "1"}
    for x, y in ink:
        if x < 7 and y < 7 and (x + 1, y + 1) not in ink:
            p[y + 1][x + 1] = 0
    for x, y in ink:
        p[y][x] = 3
    return p


# Rows 1..13 of the 16, so the digits clear the labels above and the state line below.
SEGS = {
    "a": [(x, y) for y in (1, 2) for x in range(7)],
    "b": [(x, y) for y in range(1, 8) for x in (5, 6)],
    "c": [(x, y) for y in range(6, 14) for x in (5, 6)],
    "d": [(x, y) for y in (12, 13) for x in range(7)],
    "e": [(x, y) for y in range(6, 14) for x in (0, 1)],
    "f": [(x, y) for y in range(1, 8) for x in (0, 1)],
    "g": [(x, y) for y in (6, 7) for x in range(7)],
}
DIGITS = ["abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg",
          "abcdfg", "g"]


def seven(lit):
    p = [[1] * 8 for _ in range(16)]
    for s in lit:
        for x, y in SEGS[s]:
            p[y][x] = 3
    return p


def bar_tile(n):
    p = [[1] * 8 for _ in range(8)]
    for x in range(8):
        p[1][x] = p[6][x] = 2
        for y in range(2, 6):
            p[y][x] = 3 if x < n else 0
    return p


def tri(p, x0, w, cy, h, right):
    for j in range(w):
        half = h - (h * j) // w
        x = x0 + j if right else x0 + w - 1 - j
        for y in range(cy - half, cy + half + 1):
            p[y][x] = 0


def icon(name):
    p = [[1] * 16 for _ in range(16)]
    if name == "prev":
        fill_p(p, 2, 3, 4, 12)
        tri(p, 4, 5, 7, 4, False)
        tri(p, 9, 5, 7, 4, False)
    elif name == "rew":
        tri(p, 2, 6, 7, 5, False)
        tri(p, 8, 6, 7, 5, False)
    elif name == "play":
        tri(p, 5, 8, 7, 5, True)
    elif name == "pause":
        fill_p(p, 4, 2, 7, 13)
        fill_p(p, 9, 2, 12, 13)
    elif name == "stop":
        fill_p(p, 4, 3, 12, 12)
    elif name == "ff":
        tri(p, 2, 6, 7, 5, True)
        tri(p, 8, 6, 7, 5, True)
    elif name == "next":
        tri(p, 2, 5, 7, 4, True)
        tri(p, 7, 5, 7, 4, True)
        fill_p(p, 12, 3, 14, 12)
    elif name == "eject":
        for k in range(6):
            fill_p(p, 7 - k, 3 + k, 9 + k, 4 + k)
        fill_p(p, 2, 10, 14, 12)
    return p


def fill_p(p, x0, y0, x1, y1):
    for y in range(y0, y1):
        for x in range(x0, x1):
            p[y][x] = 0


def marker():
    p = [[1] * 16 for _ in range(8)]
    for k in range(5):
        for x in range(7 - k, 9 + k):
            p[1 + k][x] = 3
    return p


fixed = {}
for code in range(0x20, 0x60):
    fixed[code] = glyph(GLYPHS.get(chr(code), GLYPHS[" "]))
for i, art in enumerate(MARKS):
    fixed[T_MARKS + i] = glyph(art)
for d, lit in enumerate(DIGITS):
    s = seven(lit)
    fixed[T_BIGT + d] = s[:8]
    fixed[T_BIGB + d] = s[8:]
for n in range(9):
    fixed[T_BAR + n] = bar_tile(n)
m = marker()
fixed[T_MARK_L], fixed[T_MARK_R] = cut(m, 0, 0), cut(m, 1, 0)
for base, name in ((T_PLAY, "play"), (T_PAUSE, "pause")):
    ic = icon(name)
    for k, (tx, ty) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
        fixed[base + k] = cut(ic, tx, ty)

# -- the picture ------------------------------------------------------------------

# Panel, rounded, outlined in light blue.
fill(8, 16, 248, 216, 1)
for x in range(8, 248):
    px(x, 16, 2)
    px(x, 215, 2)
for y in range(16, 216):
    px(8, y, 2)
    px(247, y, 2)
for x, y in ((8, 16), (247, 16), (8, 215), (247, 215)):
    px(x, y, 0)
for x, y in ((9, 17), (246, 17), (9, 214), (246, 214)):
    px(x, y, 2)


def text(col, row, s):
    for i, ch in enumerate(s):
        blit((col + i) * 8, row * 8, fixed[ord(ch)])


def big_text(col, row, s):
    for i, ch in enumerate(s):
        g = fixed[ord(ch)]
        for y in range(16):
            for x in range(16):
                px((col + 2 * i) * 8 + x, row * 8 + y, g[y // 2][x // 2])


big_text(3, 3, "NES CD PLAYER")
fill(24, 43, 232, 45, 2)

# The spectrum window: black, the bars over it are sprites.
fill(WIN_X0 + 1, VIZ_Y, WIN_X1, VIZ_Y + VIZ_ROWS * 8, 0)
for x in range(WIN_X0, WIN_X1 + 1):
    px(x, VIZ_Y - 1, 2)
    px(x, VIZ_Y + VIZ_ROWS * 8, 2)
for y in range(VIZ_Y - 1, VIZ_Y + VIZ_ROWS * 8 + 1):
    px(WIN_X0, y, 2)
    px(WIN_X1, y, 2)


# Sprite tile top * 3 + bottom, per segment 0 unlit, 1 lit, 2 peak.
def meter_tile(top, bottom):
    p = [[0] * 8 for _ in range(8)]
    for rows, s in ((range(1, 4), top), (range(5, 8), bottom)):
        for y in rows:
            for x in range(1, 7):
                p[y][x] = (3, 1, 2)[s]
    return p


sprite_tiles = [meter_tile(t, b) for t in range(3) for b in range(3)]

# The display.
pal(5, 4, 15, 7, P_LCD)
fill(81, 65, 239, 111, 1)
for x in range(81, 239):
    px(x, 65, 2)
    px(x, 110, 2)
for y in range(65, 111):
    px(81, y, 2)
    px(238, y, 2)
text(11, 9, "TRACK")
text(17, 9, "TIME")
text(13, 11, "/")
text(22, 11, "/")
text(25, 11, ":")
for col in (11, 12, 17, 18, 20, 21):
    blit(col * 8, 80, fixed[T_BIGT + 10])
    blit(col * 8, 88, fixed[T_BIGB + 10])
colon = [[1] * 8 for _ in range(16)]
for y in (4, 5, 9, 10):
    colon[y][3] = colon[y][4] = 3
blit(19 * 8, 80, colon)
text(14, 11, "--")
text(23, 11, "--")
text(26, 11, "--")

# The progress bar, capped at both ends.
for i in range(BAR_TILES):
    blit((BAR_COL + i) * 8, BAR_ROW * 8, fixed[T_BAR])
y0 = BAR_ROW * 8
lx, rx = (BAR_COL - 1) * 8 + 6, (BAR_COL + BAR_TILES) * 8 + 1
for x in (lx, lx + 1, rx - 1, rx):
    px(x, y0 + 1, 2)
    px(x, y0 + 6, 2)
for y in range(2, 6):
    px(lx, y0 + y, 2)
    px(lx + 1, y0 + y, 0)
    px(rx - 1, y0 + y, 0)
    px(rx, y0 + y, 2)

# The buttons: 32x32 bevelled keys, one attribute block pair each.
ICONS = ["prev", "rew", "play", "stop", "ff", "next", "eject"]
pal(BTN_COL // 2, BTN_ROW // 2, (BTN_COL + NBTN * BTN_PITCH) // 2, BTN_ROW // 2 + 2, P_BTN)
for i, name in enumerate(ICONS):
    x0, y0 = (BTN_COL + i * BTN_PITCH) * 8, BTN_ROW * 8
    for y in range(32):
        for x in range(32):
            if x in (0, 31) or y in (0, 31):
                c = 0
            elif x in (1, 2) or y in (1, 2):
                c = 3
            elif x in (29, 30) or y in (29, 30):
                c = 2
            else:
                c = 1
            px(x0 + x, y0 + y, c)
    for x, y in ((1, 1), (30, 1), (1, 30), (30, 30)):
        px(x0 + x, y0 + y, 0)
    blit(x0 + 8, y0 + 8, icon(name))

# -- tiles -------------------------------------------------------------------------


def key(pix):
    return tuple(c for row in pix for c in row)


ids = {}
chr_tiles = [None] * 256
for tid, pix in fixed.items():
    chr_tiles[tid] = pix
    ids.setdefault(key(pix), tid)
free = [t for t in list(range(0x89, 0x100)) + list(range(0x00, 0x10)) + list(range(0x16, 0x20))
        if chr_tiles[t] is None]

names = bytearray()
for ty in range(30):
    for tx in range(32):
        pix = cut(canvas, tx, ty)
        k = key(pix)
        if k not in ids:
            if not free:
                sys.exit("gen_gfx: out of tiles")
            t = free.pop(0)
            ids[k] = t
            chr_tiles[t] = pix
        names.append(ids[k])

attrs = bytearray(64)
for by in range(15):
    for bx in range(16):
        attrs[(by // 2) * 8 + bx // 2] |= attr[by][bx] << (((by & 1) * 2 + (bx & 1)) * 2)

blank = [[0] * 8 for _ in range(8)]
chr_bank = b"".join(encode(t or blank) for t in chr_tiles)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "gfx.chr"), "wb").write(chr_bank)
    open(os.path.join(out, "screen.bin"), "wb").write(bytes(names) + bytes(attrs))
    open(os.path.join(out, "palette.bin"), "wb").write(bytes(PALETTE))
    sprites = b"".join(encode(t) for t in sprite_tiles)
    open(os.path.join(out, "sprites.chr"), "wb").write(sprites.ljust(256, b"\0"))
    open(os.path.join(out, "vizpal.bin"), "wb").write(bytes(VIZ_PAL))
    consts = {
        "NBTN": NBTN, "BTN_PLAY": BTN_PLAY, "BTN_PITCH": BTN_PITCH,
        "T_BIGT": T_BIGT, "T_BIGB": T_BIGB, "T_BAR": T_BAR,
        "T_MARK_L": T_MARK_L, "T_MARK_R": T_MARK_R, "T_PLAY": T_PLAY, "T_PAUSE": T_PAUSE,
        "TRACK_AT": TRACK_AT, "NTRACK_AT": NTRACK_AT, "MIN_AT": MIN_AT, "SEC_AT": SEC_AT,
        "LMIN_AT": LMIN_AT, "LSEC_AT": LSEC_AT, "STATE_AT": STATE_AT, "BAR_AT": BAR_AT,
        "BAR_TILES": BAR_TILES, "MARK_AT": MARK_AT, "ICON_AT": ICON_AT,
        "PAL_FACE": 0x3F00 + P_BTN * 4 + 1,
        "VIZ_BANDS": VIZ_BANDS, "VIZ_ROWS": VIZ_ROWS, "VIZ_X": VIZ_X, "VIZ_Y": VIZ_Y,
        "VIZ_PITCH": VIZ_PITCH,
        "FACE_NORMAL": PALETTE[P_BTN * 4 + 1], "FACE_RED": FACE_RED,
    }
    with open(os.path.join(out, "gfx.inc"), "w") as f:
        f.write("; generated by gen_gfx.py\n")
        for k, v in consts.items():
            f.write(f"{k} = ${v:04X}\n")
    used = sum(t is not None for t in chr_tiles)
    print(f"gen_gfx: {used}/256 tiles, {len(free)} free")


if __name__ == "__main__":
    main()
