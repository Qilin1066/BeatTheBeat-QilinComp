#!/usr/bin/env python3
"""Differential test: lifted functions against the original code under Unicorn.

Each trial gives a function random registers (pointer-shaped values among
them) and random heap and stack contents, runs the original PowerPC code
under Unicorn's PowerPC 750CL model and the lifted C++ in qilin_difftest from
the same state, and compares everything: all GPRs, LR, CTR, CR, XER, where it
returned to, every changed byte of MEM1, and how it stopped (memory fault and
its address, trap, a call into a function that isn't lifted yet). Whole call
trees run: a lifted function's lifted callees run too, on both sides.

  difftest.py --harness build/qilin_difftest --dol main.dol --gen gen [--trials 8] [--only ADDR] [--seed N]

Needs `pip install unicorn` (2.1 or later). Trials that run too long under
Unicorn (an infinite loop on random input) are skipped, and counted.
"""
import argparse
import collections
import os
import random
import select
import re
import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_BLOCK, UC_HOOK_CODE, UC_PROT_ALL
from unicorn import ppc_const as P

MEM1, MEM1_SIZE = 0x80000000, 24 << 20
MAGIC = 0x80003000          # the return address the caller gives: zero memory below the image
STUB = 0x80002F00           # sets XER with mtxer (see Original.run), then branches to the function
STACK_TOP = 0x803E7400      # __init_registers sets r1 to 0x803E7488
R2, R13 = 0x8032A680, 0x80328500  # small-data bases, from __init_registers
HEAP, HEAP_SIZE = 0x81000000, 0x10000
STACK_FILL = 0x4000
XER_MASK = 0xE000007F
LIMIT = 400_000             # instructions per trial under Unicorn
PAGE = 4096
STATUS = ["returned", "memory fault", "bad jump", "bad return", "trap", "not lifted", "unsupported"]


def load_dol(path):
    data = open(path, "rb").read()
    image = bytearray(MEM1_SIZE)
    for i in range(18):
        off, addr, size = (struct.unpack(">I", data[o + 4 * i:o + 4 * i + 4])[0] for o in (0, 0x48, 0x90))
        if size:
            image[addr - MEM1:addr - MEM1 + size] = data[off:off + size]
    return bytes(image)


def time_readers(gen):
    """Lifted functions whose call tree reads the time base or a special register the runtime models."""
    calls, direct = {}, set()
    for name in os.listdir(gen):
        if not name.startswith("lifted_"):
            continue
        for m in re.finditer(r"void fn_([0-9A-F]{8})\(rt::Context& c\) \{\n(.*?)\n\}\n", open(os.path.join(gen, name)).read(), re.S):
            fn = int(m[1], 16)
            calls[fn] = {int(x, 16) for x in re.findall(r"fn_([0-9A-F]{8})\(c\)", m[2])}
            if "rt::mftb" in m[2] or "rt::mfspr(" in m[2]:
                direct.add(fn)
    reach, grew = set(direct), True
    while grew:
        grew = False
        for fn, callees in calls.items():
            if fn not in reach and callees & reach:
                reach.add(fn)
                grew = True
    return reach


def functions(gen):
    table = open(os.path.join(gen, "table.cpp")).read()
    stubs = open(os.path.join(gen, "stubs.cpp")).read()
    every = [int(a, 16) for a in re.findall(r"\{0x([0-9A-F]{8})u, gen::fn_", table)]
    not_lifted = {int(a, 16) for a in re.findall(r"rt::not_lifted\(0x([0-9A-F]{8})u\)", stubs)}
    return [a for a in every if a not in not_lifted], not_lifted


class Original:
    """The original code under Unicorn."""

    def __init__(self, image, not_lifted):
        self.mu = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        try:
            self.mu.ctl_set_cpu_model(P.UC_CPU_PPC32_750CL_V2_0)
        except Exception:
            pass
        self.mu.mem_map(MEM1, MEM1_SIZE, UC_PROT_ALL)
        self.mu.mem_write(MEM1, image)
        self.image = image
        self.not_lifted = not_lifted
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.mu.hook_add(UC_HOOK_BLOCK, self._block)
        # Every divw/divwu: did a trial divide by zero (or 0x80000000 by -1)?
        for off in range(0, MEM1_SIZE - 4, 4):
            w = struct.unpack(">I", image[off:off + 4])[0]
            if w >> 26 == 31 and (w >> 1) & 0x1FF in (459, 491) and (w >> 1) & 0x3FF != 0x3FF:
                self.mu.hook_add(UC_HOOK_CODE, self._divide, w, MEM1 + off, MEM1 + off)

    def _unmapped(self, mu, access, address, size, value, data):
        self.stop = (1, mu.reg_read(P.UC_PPC_REG_PC), address)
        return False

    def _divide(self, mu, address, size, word):
        a = mu.reg_read(P.UC_PPC_REG_0 + ((word >> 16) & 31))
        b = mu.reg_read(P.UC_PPC_REG_0 + ((word >> 11) & 31))
        if b == 0 or ((word >> 1) & 0x1FF == 491 and a == 0x80000000 and b == 0xFFFFFFFF):
            self.divided_by_zero = True

    def _block(self, mu, address, size, data):
        if address in self.not_lifted and address != self.entry:
            self.stop = (5, address, 0)
            mu.emu_stop()

    def run(self, fn, regs, writes):
        # Unicorn 2.1's write hook misses dcbz, stmw and stswi, so changes are
        # found by comparing all of MEM1 with the trial's starting state.
        mu = self.mu
        stub = struct.pack(">4I", 0x7C0103A6, 0x3C000000 | regs[0] >> 16, 0x60000000 | regs[0] & 0xFFFF,
                           0x48000000 | (fn - (STUB + 12)) & 0x03FFFFFC)
        writes = list(writes) + [(STUB, stub)]  # part of the starting state, like the heap and stack
        initial = bytearray(self.image)
        for addr, blob in writes:
            initial[addr - MEM1:addr - MEM1 + len(blob)] = blob
            mu.mem_write(addr, blob)
        initial = bytes(initial)
        for i in range(32):
            mu.reg_write(P.UC_PPC_REG_0 + i, regs[i])
        mu.reg_write(P.UC_PPC_REG_LR, regs[32])
        mu.reg_write(P.UC_PPC_REG_CTR, regs[33])
        mu.reg_write(P.UC_PPC_REG_CR, regs[34])
        # Unicorn 2.1's register API stores XER without setting the SO, OV
        # and CA flags the instructions use, so XER goes in through mtxer:
        # r0 carries it to the stub, which then restores r0.
        mu.reg_write(P.UC_PPC_REG_0, regs[35])
        mu.ctl_remove_cache(STUB, STUB + 16)  # it changed since Unicorn last translated it
        self.stop = None
        self.divided_by_zero = False
        self.entry = fn
        timeout = False
        try:
            mu.emu_start(STUB, MAGIC, count=LIMIT)
            pc = mu.reg_read(P.UC_PPC_REG_PC)
            if self.stop is None and pc != MAGIC:
                timeout = True
        except UcError as e:
            if self.stop is None:
                pc = mu.reg_read(P.UC_PPC_REG_PC)
                word = struct.unpack(">I", bytes(mu.mem_read(pc, 4)))[0] if MEM1 <= pc < MEM1 + MEM1_SIZE else 0
                if word >> 26 in (3, 31) and (word >> 26 == 3 or (word >> 1) & 0x3FF == 4):  # twi / tw
                    self.stop = (4, pc, 0)
                else:
                    self.stop = ("error", pc, str(e))
        status = self.stop or (0, 0, 0)
        out = [mu.reg_read(P.UC_PPC_REG_0 + i) for i in range(32)]
        out += [mu.reg_read(P.UC_PPC_REG_LR), mu.reg_read(P.UC_PPC_REG_CTR), mu.reg_read(P.UC_PPC_REG_CR), 0,
                MAGIC if status[0] == 0 else 0]
        after = bytes(mu.mem_read(MEM1, MEM1_SIZE))
        changed = {}
        for big in range(0, MEM1_SIZE, 1 << 16):
            if after[big:big + (1 << 16)] == initial[big:big + (1 << 16)]:
                continue
            for page in range(big, big + (1 << 16), PAGE):
                if after[page:page + PAGE] == initial[page:page + PAGE]:
                    continue
                for off in range(page, page + PAGE, 32):
                    if after[off:off + 32] != initial[off:off + 32]:
                        changed[MEM1 + off] = after[off:off + 32]
                mu.mem_write(MEM1 + page, self.image[page:page + PAGE])  # back to the image
        if status[0] == 0 and not timeout:  # XER out through mfxer r0 at MAGIC, for the same reason
            mu.mem_write(MAGIC, struct.pack(">I", 0x7C0102A6))
            mu.ctl_remove_cache(MAGIC, MAGIC + 4)
            mu.emu_start(MAGIC, MAGIC + 4)
            out[35] = mu.reg_read(P.UC_PPC_REG_0)
            mu.mem_write(MAGIC, self.image[MAGIC - MEM1:MAGIC - MEM1 + 4])
        for addr, blob in writes:  # the trial's own writes, back to the image too
            mu.mem_write(addr, self.image[addr - MEM1:addr - MEM1 + len(blob)])
        if timeout:
            return None
        return status, out, changed


class Lifted:
    """The lifted code in qilin_difftest."""

    def __init__(self, harness, dol):
        self.args = [harness, dol]
        self.start()

    def start(self):
        self.p = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def run(self, fn, regs, writes, timeout=20):
        """The result, or None if the lifted code didn't finish in time (the harness is restarted)."""
        msg = struct.pack("<37I", fn, *regs) + struct.pack("<I", len(writes))
        for addr, blob in writes:
            msg += struct.pack("<II", addr, len(blob)) + blob
        self.p.stdin.write(msg)
        self.p.stdin.flush()
        if not select.select([self.p.stdout], [], [], timeout)[0]:
            self.p.kill()
            self.p.wait()
            self.start()
            return None
        rd = self.p.stdout.read
        status = struct.unpack("<3I", rd(12))
        out = list(struct.unpack("<37I", rd(37 * 4)))
        n = struct.unpack("<I", rd(4))[0]
        changed = {}
        for _ in range(n):
            addr = struct.unpack("<I", rd(4))[0]
            changed[addr] = rd(32)
        return status, out, changed


def random_state(rng, image):
    def value():
        k = rng.random()
        if k < 0.35:
            return HEAP + rng.randrange(0, HEAP_SIZE - 0x100) & ~3
        if k < 0.6:
            return rng.randrange(0, 256)
        if k < 0.7:
            return R13 + rng.randrange(-0x8000, 0x8000) & ~3
        return rng.getrandbits(32)
    regs = [value() for _ in range(32)]
    regs[1] = STACK_TOP - 8 * rng.randrange(0, 16)
    regs[2], regs[13] = R2, R13
    regs += [MAGIC, rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32) & XER_MASK]
    heap = bytearray(rng.getrandbits(8) for _ in range(HEAP_SIZE))
    for off in range(0, HEAP_SIZE, 4):  # a third of the heap's words point back into it
        if rng.random() < 0.33:
            heap[off:off + 4] = struct.pack(">I", HEAP + rng.randrange(0, HEAP_SIZE - 0x100) & ~3)
    stack = bytes(rng.getrandbits(8) for _ in range(STACK_FILL))
    return regs, [(HEAP, bytes(heap)), (STACK_TOP - STACK_FILL, stack)]


# Known differences, by design. Each returns a reason or None.
CACHE_OPS = {(31, 86), (31, 54), (31, 278), (31, 246), (31, 982)}  # dcbf dcbst dcbt dcbtst icbi


def known_difference(a, b, image, divided_by_zero, reads_time):
    (sa, _, _), (sb, _, _) = a, b
    if divided_by_zero:
        return ("divide by zero: undefined in the architecture; lifted code gives Broadway's result, "
                "Unicorn leaves the dividend")
    if reads_time:
        return "the call tree reads the time base or a special register, which differ from Unicorn's by design"
    if sa[0] == 1 and MEM1 <= sa[1] < MEM1 + MEM1_SIZE:
        word = struct.unpack(">I", image[sa[1] - MEM1:sa[1] - MEM1 + 4])[0]
        if (word >> 26, (word >> 1) & 0x3FF) in CACHE_OPS:
            return "cache operation on an unmapped address: no-op when lifted, a fault under Unicorn"
    if sa[0] == 1 and sb[0] == 1 and sa[1] == sb[1] and sa[2] >> 5 == sb[2] >> 5:
        word = struct.unpack(">I", image[sa[1] - MEM1:sa[1] - MEM1 + 4])[0]
        if word >> 26 == 31 and (word >> 1) & 0x3FF == 1014:
            return "dcbz fault: same instruction and block; Unicorn reports where its zeroing loop faulted"
    if sb[0] == 2:
        return "indirect jump to an address that isn't a function: lifted code stops, Unicorn runs whatever is there"
    return None


def compare(a, b):
    """'' if equal, else what differs."""
    (sa, oa, ma), (sb, ob, mb) = a, b
    if sa[0] != sb[0]:
        return f"stopped differently: original {STATUS[sa[0]] if isinstance(sa[0], int) else sa}, lifted {STATUS[sb[0]]} ({sb[1]:08X} {sb[2]:08X})"
    if sa[0] in (1, 5) and (sa[1], sa[2]) != (sb[1], sb[2]):
        return f"{STATUS[sa[0]]} at {sa[1]:08X}/{sa[2]:08X} vs {sb[1]:08X}/{sb[2]:08X}"
    if sa[0] != 0:
        return ""  # both stopped the same way; registers aren't synced at a stop
    names = [f"r{i}" for i in range(32)] + ["lr", "ctr", "cr", "xer", "ret"]
    diffs = []
    for i, n in enumerate(names):
        x, y = oa[i], ob[i]
        if n == "xer":
            x, y = x & XER_MASK, y & XER_MASK
        if x != y:
            diffs.append(f"{n} {x:08X}/{y:08X}")
    if ma != mb:
        addrs = sorted(set(ma) ^ set(mb) | {k for k in ma if k in mb and ma[k] != mb[k]})
        diffs.append(f"memory at {', '.join(f'{x:08X}' for x in addrs[:4])}")
    return "; ".join(diffs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--trials", type=int, default=8)
    ap.add_argument("--only", help="one function (hex address)")
    ap.add_argument("--first", type=int, help="only the first N lifted functions")
    ap.add_argument("--seed", type=int, default=750)
    ap.add_argument("--show", type=int, default=30, help="mismatching functions to print")
    args = ap.parse_args()

    image = load_dol(args.dol)
    lifted, not_lifted = functions(args.gen)
    if args.only:
        lifted = [int(args.only, 16)]
    if args.first:
        lifted = lifted[:args.first]
    original = Original(image, not_lifted)
    time = time_readers(args.gen)
    harness = Lifted(args.harness, args.dol)
    rng = random.Random(args.seed)
    counts = collections.Counter()
    stops = collections.Counter()
    bad = {}
    for k, fn in enumerate(lifted):
        for _ in range(args.trials):
            regs, writes = random_state(rng, image)
            a = original.run(fn, regs, writes)
            if a is None:
                counts["skipped: runs too long under Unicorn"] += 1
                continue
            b = harness.run(fn, regs, writes)
            if b is None:
                counts["MISMATCH"] += 1
                bad.setdefault(fn, "lifted code didn't finish; the original did")
                continue
            if a[0][0] == "error":
                counts["skipped: Unicorn stopped (" + a[0][2].split('(')[0].strip() + ")"] += 1
                continue
            if b[0][0] == 6:
                counts["skipped: unsupported special register or system call"] += 1
                continue
            why = compare(a, b)
            known = known_difference(a, b, image, original.divided_by_zero, fn in time) if why else None
            if known:
                counts["known: " + known] += 1
            elif why:
                counts["MISMATCH"] += 1
                bad.setdefault(fn, why)
            else:
                counts["agree"] += 1
                stops[STATUS[a[0][0]]] += 1
        if (k + 1) % 500 == 0:
            print(f"  {k + 1}/{len(lifted)} functions, {counts['MISMATCH']} mismatching trials", file=sys.stderr)
    total = sum(counts.values())
    print(f"{len(lifted)} functions, {total} trials:")
    for key, n in sorted(counts.items()):
        print(f"  {n:7} {key}")
    print("  agreeing trials ended: " + ", ".join(f"{STATUS_LABEL}: {n}" for STATUS_LABEL, n in stops.most_common()))
    print(f"{len(bad)} functions with a mismatch")
    for fn, why in list(bad.items())[:args.show]:
        print(f"  {fn:08X} {why}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
