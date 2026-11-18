#!/usr/bin/env python3
"""The boot values a console leaves in low memory, from a Dolphin MEM1 dump.

  lowmem.py mem1.raw

IOS and the apploader fill in low memory (0x80000000-0x80003400) before the
game's entry point runs; OSInit and __start read it, and the runtime has to
put the same values there (docs/runtime.md). Nothing in main.dol says what
they are, so they come from the reference: dump MEM1 in Dolphin with the game
stopped at its entry point (0x80004050) and run this on the dump.

It prints each word OSInit or __start reads in the supported build, with the
value the runtime uses today, and flags the ones that differ.

The dump holds the whole game image: keep it out of git (*.raw is ignored).
"""
import argparse
import struct
import sys

MEM1 = 0x80000000
MEM1_SIZE = 24 << 20

# (address, size, what reads it and why, the runtime's value or None).
# Addresses are the ones the code reads (see docs/runtime.md); the
# descriptions say what the code does with each, not more.
FIELDS = [
    (0x80000028, 4, "physical memory size", None),
    (0x8000002C, 4, "console type", None),
    (0x80000030, 4, "boot info: arena start (OSInit: used when 0x8000310C is 0 and this isn't)", None),
    (0x80000034, 4, "boot info: arena end (OSInit: MEM1 arena end if 0x80003110 is 0; __start: debugger check)", None),
    (0x800000F4, 4, "pointer to bi2 (debug flags; __start and OSInit read through it)", None),
    (0x800000F8, 4, "bus clock in Hz (the SDK's tick conversions)", None),
    (0x800000FC, 4, "CPU clock in Hz", None),
    (0x800030E6, 2, "device code (OSInit: 0 makes it ask the drive, DVDInquiryAsync)", None),
    (0x800030E8, 1, "debug monitor byte (__start, OSInit)", None),
    (0x8000310C, 4, "MEM1 arena start (OSInit's first choice)", 0x803E94A0),
    (0x80003110, 4, "MEM1 arena end (OSInit's first choice)", 0x81730000),
    (0x80003124, 4, "MEM2 arena start (OSInit: raised to 0x90000800 if lower)", 0x90000800),
    (0x80003128, 4, "MEM2 arena end", 0x933E0000),
    (0x80003140, 4, "IOS version (OSInit reads it through the uncached mirror)", None),
    (0x80003144, 4, "IOS build date, BCD (OSInit decodes it for its report)", None),
    (0x8000315C, 1, "OSInit: 0x81 stops the game (OSPanic)", None),
    (0x8000315D, 1, "OSInit: 0x81 stops the game (OSPanic)", None),
    (0x80003184, 1, "OSInit: 0x80 makes it check the disc device", None),
    (0x80003188, 4, "OSInit: compared with the IOS version (too old: OSFatal)", None),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="MEM1 dump from Dolphin (24 MB, starting at 0x80000000)")
    args = ap.parse_args()

    data = open(args.dump, "rb").read()
    if len(data) < 0x3400:
        sys.exit(f"lowmem: {args.dump} is {len(data)} bytes; a MEM1 dump is {MEM1_SIZE}")
    if len(data) != MEM1_SIZE:
        print(f"note: {args.dump} is {len(data)} bytes, not MEM1's {MEM1_SIZE}; reading it from 0x80000000 anyway")
    # The game's ID at 0x80000000 says the dump is of this game.
    disc = data[0:6].decode("ascii", "replace")
    if disc != "SOMP01":
        print(f"warning: the dump starts with {disc!r}, not SOMP01 (the supported build)")

    differ = 0
    print(f"{'address':10} {'value':>10}  {'runtime':>10}  what")
    for addr, size, what, ours in FIELDS:
        off = addr - MEM1
        v = int.from_bytes(data[off:off + size], "big")
        width = 2 * size
        mark = ""
        if ours is not None and v != ours:
            mark = "  <-- differs"
            differ += 1
        ours_s = f"{ours:0{width}X}" if ours is not None else "-"
        v_s = f"{v:0{width}X}"
        print(f"{addr:08X}   {v_s:>10}  {ours_s:>10}  {what}{mark}")

    # bi2's flags, where OSInit and __start read them.
    bi2 = int.from_bytes(data[0xF4:0xF8], "big")
    if MEM1 <= bi2 < MEM1 + len(data) - 0x28:
        o = bi2 - MEM1
        print(f"\nbi2 at {bi2:08X}: +0x0C {struct.unpack('>I', data[o + 0xC:o + 0x10])[0]:08X}, "
              f"+0x24 {struct.unpack('>I', data[o + 0x24:o + 0x28])[0]:08X}")
    print(f"\n{differ} of the runtime's values differ from the dump"
          + (": update kMem1ArenaLo and friends in recomp/runtime/os.cpp" if differ else ""))
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
