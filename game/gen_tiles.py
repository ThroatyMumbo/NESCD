#!/usr/bin/env python3
import sys

BANK = 8192


def encode(rows):
    if len(rows) != 8 or any(len(r) != 8 for r in rows):
        raise ValueError("a tile is 8x8")
    p0 = bytes(sum(((v & 1) << (7 - x)) for x, v in enumerate(r)) for r in rows)
    p1 = bytes(sum((((v >> 1) & 1) << (7 - x)) for x, v in enumerate(r))
               for r in rows)
    return p0 + p1


def tile(art, key):
    return encode([[key[c] for c in line] for line in art])


BLANK = tile(["........"] * 8, {".": 0})

FLOOR = tile([
    "........",
    "........",
    "..1.....",
    "........",
    "........",
    "......1.",
    "........",
    "........",
], {".": 0, "1": 1})

WALL = tile([
    "22222222",
    "22222222",
    "22222222",
    "........",
    "22222222",
    "22222222",
    "22222222",
    "........",
], {".": 0, "2": 2})

TRIGGER = tile([
    "33333333",
    "3......3",
    "3.3333.3",
    "3.3333.3",
    "3.3333.3",
    "3......3",
    "33333333",
    "..3..3..",
], {".": 0, "3": 3})

PLAYER = tile([
    "..3333..",
    ".311113.",
    "31133113",
    "31111113",
    "31111113",
    "31122113",
    ".311113.",
    "..3333..",
], {".": 0, "1": 1, "2": 2, "3": 3})

GLYPH_KEY = {".": 0, "3": 3}

# Tiles $04..$08: the digit over each cue tile, 0 (stop) through 4.
DIGITS = [tile(a, GLYPH_KEY) for a in (
    [".33333..", "3.....3.", "3.....3.", "3.....3.", "3.....3.", "3.....3.", ".33333..", "........"],
    ["...3....", "..33....", "...3....", "...3....", "...3....", "...3....", ".33333..", "........"],
    [".33333..", "3.....3.", "......3.", ".....3..", "...33...", ".33.....", "3333333.", "........"],
    [".33333..", "3.....3.", "......3.", "...333..", "......3.", "3.....3.", ".33333..", "........"],
    ["....33..", "...3.3..", "..3..3..", ".3...3..", "3333333.", ".....3..", ".....3..", "........"],
)]


def main():
    out = sys.argv[1]
    bank = bytearray(BANK)
    tiles = [BLANK, FLOOR, WALL, TRIGGER] + DIGITS
    for i, t in enumerate(tiles):
        bank[i * 16:(i + 1) * 16] = t
    bank[0x1000:0x1010] = PLAYER
    open(out, "wb").write(bank)
    print(f"{out}: {BANK} bytes")


if __name__ == "__main__":
    main()
