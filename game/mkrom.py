#!/usr/bin/env python3
"""Wrap the ld65 bank into a flashable .nes: iNES header + the 32 KiB bank.

NROM-256 with 8 KiB CHR RAM, so a stock EverDrive N8 Pro core runs it. The
game uploads its own tiles at boot; CHR ROM size 0 is what asks for the RAM.
"""
import argparse

BANK = 32768

# byte4 = 2      32 KiB PRG
# byte5 = 0      no CHR ROM, so 8 KiB CHR RAM
# byte6 = 0      horizontal mirroring, no battery, no four-screen
# byte7 = 0      mapper 0, iNES 1.0
HEADER = bytes([0x4E, 0x45, 0x53, 0x1A, BANK // 16384, 0, 0, 0]) + bytes(8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bank", help="32 KiB $8000-$FFFF image from ld65")
    ap.add_argument("out")
    a = ap.parse_args()

    bank = open(a.bank, "rb").read()
    if len(bank) != BANK:
        raise SystemExit(f"error: {a.bank} is {len(bank)} bytes, want {BANK}")

    img = HEADER + bank
    open(a.out, "wb").write(img)
    print(f"{a.out}: {len(img)} bytes (mapper 0, 32 KiB PRG + 8 KiB CHR RAM)")


if __name__ == "__main__":
    main()
