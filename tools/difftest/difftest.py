#!/usr/bin/env python3
"""Differential test: lifted functions against the original code under Unicorn.

Each trial gives a function random registers (pointer-shaped values among
them) and random heap and stack contents, runs the original PowerPC code
under Unicorn's PowerPC 750CL model and the lifted C++ in qilin_difftest from
the same state, and compares everything: all GPRs, LR, CTR, CR, XER, where it
returned to, every changed byte of MEM1, and how it stopped (memory fault and
its address, trap, a call into a function that isn't lifted yet). Whole call
trees run: a lifted function's lifted callees run too, on both sides.

  difftest.py --harness build/qilin_difftest --dol main.dol --gen gen
              [--trials 8] [--seed N] [--jobs N]
              [--only ADDR[,ADDR...]] [--range LO-HI] [--first N]

Functions are shared out among --jobs workers (default: one per CPU), each
with its own Unicorn and its own qilin_difftest. A function's trials are drawn
from a generator seeded with --seed and the function's address alone, so they
are the same whatever the number of workers, the order, or the selection:
`--only` on a mismatching function replays exactly the trials of the full run.

Needs `pip install unicorn` (2.1 or later). Runs on Linux, macOS and Windows.
Trials that run too long under Unicorn (an infinite loop on random input) are
skipped, and counted.
"""
import argparse
import array
import collections
import ctypes
import multiprocessing
import os
import queue
import random
import re
import struct
import subprocess
import sys
import threading
import time

from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_BLOCK, UC_HOOK_CODE, UC_PROT_ALL
from unicorn import ppc_const as P

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # paired.py, under python -I too
import paired  # noqa: E402

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

memcmp = ctypes.cdll.msvcrt.memcmp if sys.platform == "win32" else ctypes.CDLL(None).memcmp
memcmp.restype, memcmp.argtypes = ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]


def load_dol(path):
    """MEM1 holding the DOL image, and the text sections as (lo, hi) addresses."""
    data = open(path, "rb").read()
    image = bytearray(MEM1_SIZE)
    text = []
    for i in range(18):
        off, addr, size = (struct.unpack(">I", data[o + 4 * i:o + 4 * i + 4])[0] for o in (0, 0x48, 0x90))
        if size:
            image[addr - MEM1:addr - MEM1 + size] = data[off:off + size]
            if i < 7:
                text.append((addr, addr + size))
    return bytes(image), text


def functions(gen):
    table = open(os.path.join(gen, "table.cpp")).read()
    stubs = open(os.path.join(gen, "stubs.cpp")).read()
    every = [int(a, 16) for a in re.findall(r"\{0x([0-9A-F]{8})u, gen::fn_", table)]
    not_lifted = {int(a, 16) for a in re.findall(r"rt::not_lifted\(0x([0-9A-F]{8})u\)", stubs)}
    return [a for a in every if a not in not_lifted], not_lifted


class Original:
    """The original code under Unicorn."""

    def __init__(self, image, text, not_lifted):
        self.mu = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        try:
            self.mu.ctl_set_cpu_model(P.UC_CPU_PPC32_750CL_V2_0)
        except Exception:
            pass
        # MEM1 lives in a buffer of ours, so finding what a trial changed is a
        # memcmp against its starting state: no 24 MB read out of Unicorn.
        self.mem = (ctypes.c_ubyte * MEM1_SIZE).from_buffer_copy(image)
        self.mu.mem_map_ptr(MEM1, MEM1_SIZE, UC_PROT_ALL, ctypes.addressof(self.mem))
        self.image = image
        self.initial = bytearray(image)  # a trial's starting state; the image again between trials
        self.initial_at = ctypes.addressof((ctypes.c_ubyte * MEM1_SIZE).from_buffer(self.initial))
        self.not_lifted = not_lifted
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.mu.hook_add(UC_HOOK_BLOCK, self._block)
        # Every divw/divwu: did a trial divide by zero (or 0x80000000 by -1)?
        # Every fctiwz, and the instruction after it: see _fctiwz.
        self.fctiwz, self.divides = {}, {}
        for lo, hi in text:
            for i, (w,) in enumerate(struct.iter_unpack(">I", image[lo - MEM1:hi - MEM1])):
                if w >> 26 == 31 and (w >> 1) & 0x1FF in (459, 491) and (w >> 1) & 0x3FF != 0x3FF:
                    self.divides[lo + 4 * i] = w
                if w >> 26 == 63 and (w >> 1) & 0x3FF == 15:
                    self.fctiwz[lo + 4 * i] = ((w >> 21) & 31, (w >> 11) & 31)
        # Paired singles, which Unicorn lacks: the model in paired.py.
        self.kind = {}
        for lo, hi in text:
            for i, (w,) in enumerate(struct.iter_unpack(">I", image[lo - MEM1:hi - MEM1])):
                k = paired.classify(w)
                if k:
                    self.kind[lo + 4 * i] = (k, w)
        self.model = paired.Paired(self.mu, P, self._read, self._write)
        after = {a + 4 for a in self.fctiwz} | {a + 4 for a, (k, _) in self.kind.items() if k == "dup"}
        # One code hook per text section, filtered here: Unicorn scans its
        # list of code hooks linearly, and one hook per address (thousands)
        # made trials 30 times slower than this.
        # Instructions whose results differ from Unicorn's by design: the time
        # base, and special registers the runtime models (other than LR, CTR,
        # XER and the GQRs, which the model above keeps). A trial that runs
        # one is flagged, whatever path got it there.
        self.time = set()
        for lo, hi in text:
            for i, (w,) in enumerate(struct.iter_unpack(">I", image[lo - MEM1:hi - MEM1])):
                if w >> 26 == 31 and (w >> 1) & 0x3FF == 371:  # mftb
                    self.time.add(lo + 4 * i)
                elif w >> 26 == 31 and (w >> 1) & 0x3FF == 339:  # mfspr
                    spr = ((w >> 16) & 31) | ((w >> 11) & 31) << 5
                    if spr not in (1, 8, 9) and not 912 <= spr <= 919:
                        self.time.add(lo + 4 * i)
        self.hooked = set(self.fctiwz) | set(self.kind) | after | set(self.divides) | self.time
        for lo, hi in text:
            self.mu.hook_add(UC_HOOK_CODE, self._code, None, lo, hi - 4)

    def _unmapped(self, mu, access, address, size, value, data):
        self.stop = (1, mu.reg_read(P.UC_PPC_REG_PC), address)
        return False

    def _divide(self, mu, address, size, word):
        a = mu.reg_read(P.UC_PPC_REG_0 + ((word >> 16) & 31))
        b = mu.reg_read(P.UC_PPC_REG_0 + ((word >> 11) & 31))
        if b == 0 or ((word >> 1) & 0x1FF == 491 and a == 0x80000000 and b == 0xFFFFFFFF):
            self.divided_by_zero = True

    def _read(self, ea, n):
        if MEM1 <= ea and ea + n <= MEM1 + MEM1_SIZE:
            return bytes(self.mu.mem_read(ea, n))
        return None

    def _write(self, ea, blob):
        if MEM1 <= ea and ea + len(blob) <= MEM1 + MEM1_SIZE:
            self.mu.mem_write(ea, blob)
            return True
        return False

    def _code(self, mu, address, size, data):
        if address not in self.hooked:
            return
        if address in self.divides:
            self._divide(mu, address, size, self.divides[address])
        if address in self.time:
            self.read_time = True
        if self.dup is not None:  # the instruction before wrote both halves
            self.model.ps1[self.dup] = mu.reg_read(P.UC_PPC_REG_FPR0 + self.dup)
            self.dup = None
        self._fctiwz(mu, address)
        k = self.kind.get(address)
        if k is None:
            return
        kind, w = k
        if kind == "dup":
            self.dup = (w >> 21) & 31
            return
        stop = paired.execute_spr(self.model, address, w) if w >> 26 == 31 else self.model.execute(address, w)
        if stop is not None:
            self.stop = stop
            mu.emu_stop()
            return
        mu.reg_write(P.UC_PPC_REG_PC, address + 4)  # emulated: skip it

    def _fctiwz(self, mu, address):
        # QEMU 5 (Unicorn 2.1) leaves fctiwz's high word 0, which the
        # architecture leaves undefined and the 750 family sets to 0xFFF80000,
        # and gives 0x80000000 for a positive overflow, where the architecture
        # says 0x7FFFFFFF. The hook on the next instruction puts both right.
        if address not in self.fctiwz and self.pending is None:
            return
        if self.pending is not None:
            frd, overflow = self.pending
            low = 0x7FFFFFFF if overflow else mu.reg_read(P.UC_PPC_REG_FPR0 + frd) & 0xFFFFFFFF
            mu.reg_write(P.UC_PPC_REG_FPR0 + frd, 0xFFF8000000000000 | low)
            self.pending = None
        if address in self.fctiwz:
            frd, frb = self.fctiwz[address]
            b = struct.unpack("<d", struct.pack("<Q", mu.reg_read(P.UC_PPC_REG_FPR0 + frb)))[0]
            self.pending = (frd, b == b and b >= 2147483648.0)

    def _block(self, mu, address, size, data):
        if address in self.not_lifted and address != self.entry:
            self.stop = (5, address, 0)
            mu.emu_stop()

    def run(self, fn, regs, fp, writes):
        fprs, ps1, gqr = fp
        # Unicorn 2.1's write hook misses dcbz, stmw and stswi, so changes are
        # found by comparing all of MEM1 with the trial's starting state.
        mu = self.mu
        initial = self.initial
        for addr, blob in writes:
            initial[addr - MEM1:addr - MEM1 + len(blob)] = blob
            mu.mem_write(addr, blob)
        for i in range(32):
            mu.reg_write(P.UC_PPC_REG_0 + i, regs[i])
        mu.reg_write(P.UC_PPC_REG_LR, regs[32])
        mu.reg_write(P.UC_PPC_REG_CTR, regs[33])
        mu.reg_write(P.UC_PPC_REG_CR, regs[34])
        mu.reg_write(P.UC_PPC_REG_MSR, mu.reg_read(P.UC_PPC_REG_MSR) | 0x2000)  # MSR[FP]: the FPU is available
        for i in range(32):
            mu.reg_write(P.UC_PPC_REG_FPR0 + i, fprs[i])
        self.pending = None
        self.dup = None
        self.model.ps1 = list(ps1)
        self.model.gqr = list(gqr)
        # Unicorn 2.1's register API stores XER without setting the SO, OV
        # and CA flags the instructions use, so XER goes in through mtxer:
        # r0 carries it to the stub, which then restores r0.
        mu.reg_write(P.UC_PPC_REG_0, regs[35])
        mu.ctl_remove_cache(STUB, STUB + 16)  # it changed since Unicorn last translated it
        self.stop = None
        self.divided_by_zero = False
        self.read_time = False
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
        mem_at, initial_at = ctypes.addressof(self.mem), self.initial_at
        changed = {}
        for big in range(0, MEM1_SIZE, 1 << 16):
            if not memcmp(mem_at + big, initial_at + big, 1 << 16):
                continue
            for page in range(big, big + (1 << 16), PAGE):
                if not memcmp(mem_at + page, initial_at + page, PAGE):
                    continue
                after = ctypes.string_at(mem_at + page, PAGE)
                for off in range(0, PAGE, 32):
                    if after[off:off + 32] != initial[page + off:page + off + 32]:
                        changed[MEM1 + page + off] = after[off:off + 32]
                mu.mem_write(MEM1 + page, self.image[page:page + PAGE])  # back to the image
        if status[0] == 0 and not timeout:  # XER out through mfxer r0 at MAGIC, for the same reason
            mu.mem_write(MAGIC, struct.pack(">I", 0x7C0102A6))
            mu.ctl_remove_cache(MAGIC, MAGIC + 4)
            mu.emu_start(MAGIC, MAGIC + 4)
            out[35] = mu.reg_read(P.UC_PPC_REG_0)
            mu.mem_write(MAGIC, self.image[MAGIC - MEM1:MAGIC - MEM1 + 4])
        for addr, blob in writes:  # the trial's own writes, back to the image too, on both copies
            original = self.image[addr - MEM1:addr - MEM1 + len(blob)]
            mu.mem_write(addr, original)
            initial[addr - MEM1:addr - MEM1 + len(blob)] = original
        if timeout:
            return None
        return status, out, changed, [mu.reg_read(P.UC_PPC_REG_FPR0 + i) for i in range(32)], 0, list(self.model.ps1)


class Lifted:
    """The lifted code in qilin_difftest. A reader thread collects its results,
    so a hung harness can be timed out the same way on every host."""

    def __init__(self, harness, dol):
        self.args = [os.path.abspath(harness), dol]  # Windows won't resolve a relative path with '/'
        self.start()

    def start(self):
        self.p = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.results = queue.Queue()
        threading.Thread(target=self._read, args=(self.p.stdout, self.results), daemon=True).start()

    @staticmethod
    def _read(stream, results):
        def rd(n):
            b = stream.read(n)
            if len(b) != n:
                raise EOFError
            return b
        try:
            while True:
                status = struct.unpack("<3I", rd(12))
                out = list(struct.unpack("<37I", rd(37 * 4)))
                fprs = list(struct.unpack("<32Q", rd(32 * 8)))
                ps1 = list(struct.unpack("<32Q", rd(32 * 8)))
                flushes = struct.unpack("<I", rd(4))[0]
                n = struct.unpack("<I", rd(4))[0]
                changed = {}
                for _ in range(n):
                    addr = struct.unpack("<I", rd(4))[0]
                    changed[addr] = rd(32)
                results.put((status, out, changed, fprs, flushes, ps1))
        except (EOFError, OSError, ValueError):
            results.put(None)  # the harness exited (or was killed)

    def run(self, fn, regs, fp, writes, timeout=20):
        """The result, or None if the lifted code didn't finish in time (the harness is restarted)."""
        fprs, ps1, gqr = fp
        msg = (struct.pack("<37I", fn, *regs) + struct.pack("<32Q", *fprs) + struct.pack("<32Q", *ps1) +
               struct.pack("<8I", *gqr) + struct.pack("<I", len(writes)))
        for addr, blob in writes:
            msg += struct.pack("<II", addr, len(blob)) + blob
        self.p.stdin.write(msg)
        self.p.stdin.flush()
        try:
            result = self.results.get(timeout=timeout)
        except queue.Empty:
            self.p.kill()
            self.p.wait()
            self.start()
            return None
        if result is None:  # a crash in lifted code is a bug in it or in the lifter: stop
            raise RuntimeError(f"qilin_difftest exited with status {self.p.wait()} running {fn:08X}")
        return result


def random_state(rng):
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
    # A third of the heap's words (84 in 256) point back into it, uniformly
    # below its last 256 bytes. Drawn in bulk: per-word draws were most of a
    # trial's time. Hosts are little-endian (see guest.hpp), so the arrays'
    # byte order, and with it the trials, are the same on every host.
    words = HEAP_SIZE // 4
    heap = array.array("I", rng.randbytes(HEAP_SIZE))
    pick, offset = rng.randbytes(words), array.array("H", rng.randbytes(2 * words))
    for i in [i for i, p in enumerate(pick) if p < 84]:
        heap[i] = HEAP + ((offset[i] * (HEAP_SIZE - 0x100)) >> 16 & ~3)
    heap.byteswap()  # guest words are big-endian
    stack = rng.randbytes(STACK_FILL)
    fp = ([fpr_value(rng) for _ in range(32)], [fpr_value(rng) for _ in range(32)], gqr_values(rng))
    return regs, fp, [(HEAP, heap.tobytes()), (STACK_TOP - STACK_FILL, stack)]


def gqr_values(rng):
    """GQR0 is 0 (single, unscaled) most of the time, as the OS sets it and
    compiled code assumes; the rest get random types and scales (no reserved
    type)."""
    def one():
        def half():
            return rng.choice([0, 0, 4, 5, 6, 7]), rng.choice([0, 0, rng.randrange(-8, 9), rng.randrange(-32, 32)]) & 0x3F
        lt, ls = half()
        st, ss = half()
        return ls << 24 | lt << 16 | ss << 8 | st
    g = [one() for _ in range(8)]
    if rng.random() < 0.7:
        g[0] = 0
    return g


def fpr_value(rng):
    """An FPR's bits: mostly singles, as game code holds, and some of everything else."""
    def bits(x):
        return struct.unpack("<Q", struct.pack("<d", x))[0]
    k = rng.random()
    if k < 0.45:  # a single, as lfs and single-precision arithmetic leave
        return bits(struct.unpack("<f", struct.pack("<f", rng.uniform(-1000.0, 1000.0) * 2.0 ** rng.randrange(-20, 20)))[0])
    if k < 0.6:
        return bits(rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, -2.5, 100.0, 2147483648.0, -2147483649.0]))
    if k < 0.75:  # a double that isn't a single
        return bits(rng.uniform(-1e6, 1e6))
    if k < 0.85:  # infinities, NaNs (quiet, signalling, with payloads), denormals
        return rng.choice([0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000001234,
                           0x7FF4000000000000, 0x7FF0000000000001, 0x0000000000000001, 0x8008000000000000,
                           bits(1e-40), bits(-3e-39)])
    return rng.getrandbits(64)


# Known differences, by design. Each returns a reason or None.
CACHE_OPS = {(31, 86), (31, 54), (31, 278), (31, 246), (31, 982)}  # dcbf dcbst dcbt dcbtst icbi


def known_difference(a, b, image, divided_by_zero, reads_time):
    sa, sb = a[0], b[0]
    if b[4]:
        return "non-IEEE mode flushed a denormal to zero (OSInit sets FPSCR[NI]); Unicorn ignores NI"
    if divided_by_zero:
        return ("divide by zero: undefined in the architecture; lifted code gives Broadway's result, "
                "Unicorn leaves the dividend")
    if reads_time:
        return "the trial read the time base or a special register, which differ from Unicorn's by design"
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
    if sb[0] == 3:
        return ("return to an address other than the caller's (a stack smashed by random input): "
                "lifted code stops, Unicorn jumps there")
    return None


def compare(a, b):
    """'' if equal, else what differs."""
    (sa, oa, ma, fa, _, qa), (sb, ob, mb, fb, _, qb) = a, b
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
    for i in range(32):
        if fa[i] != fb[i]:
            diffs.append(f"f{i} {fa[i]:016X}/{fb[i]:016X}")
        if qa[i] != qb[i]:
            diffs.append(f"ps1 f{i} {qa[i]:016X}/{qb[i]:016X}")
    if ma != mb:
        addrs = sorted(set(ma) ^ set(mb) | {k for k in ma if k in mb and ma[k] != mb[k]})
        diffs.append(f"memory at {', '.join(f'{x:08X}' for x in addrs[:4])}")
    return "; ".join(diffs)


# ---- Workers: each holds one Unicorn and one qilin_difftest for its lifetime ----
_worker = {}


def init_worker(harness, dol, not_lifted, trials, seed):
    image, text = load_dol(dol)
    _worker.update(original=Original(image, text, not_lifted), lifted=Lifted(harness, dol), image=image,
                   trials=trials, seed=seed)


def test_function(fn):
    """All of one function's trials: (fn, outcome counts, how agreeing trials ended, first mismatch or None)."""
    w = _worker
    rng = random.Random(w["seed"] << 32 | fn)  # this function's trials, whatever else runs
    counts, stops, bad = collections.Counter(), collections.Counter(), None
    for _ in range(w["trials"]):
        regs, fp, writes = random_state(rng)
        # Unicorn enters through the stub (see Original.run). It is part of the
        # starting state on both sides, like the heap and stack: a function
        # that reads memory down to it must see the same bytes either way.
        writes.append((STUB, struct.pack(">4I", 0x7C0103A6, 0x3C000000 | regs[0] >> 16, 0x60000000 | regs[0] & 0xFFFF,
                                         0x48000000 | (fn - (STUB + 12)) & 0x03FFFFFC)))
        a = w["original"].run(fn, regs, fp, writes)
        if a is None:
            counts["skipped: runs too long under Unicorn"] += 1
            continue
        b = w["lifted"].run(fn, regs, fp, writes)
        if b is None:
            counts["MISMATCH"] += 1
            bad = bad or "lifted code didn't finish; the original did"
            continue
        if a[0][0] == "error":
            counts["skipped: Unicorn stopped (" + a[0][2].split('(')[0].strip() + ")"] += 1
            continue
        if b[0][0] == 6:
            counts["skipped: " + ("a floating-point case Broadway's result isn't pinned down for"
                                  if b[0][2] == 0xFFFFFFFF else "unsupported special register or system call")] += 1
            continue
        why = compare(a, b)
        known = known_difference(a, b, w["image"], w["original"].divided_by_zero, w["original"].read_time) if why else None
        if known:
            counts["known: " + known] += 1
        elif why:
            counts["MISMATCH"] += 1
            bad = bad or why
        else:
            counts["agree"] += 1
            stops[STATUS[a[0][0]]] += 1
    return fn, counts, stops, bad


def parse_hex_range(text):
    lo, sep, hi = text.partition("-")
    if not sep:
        raise argparse.ArgumentTypeError("--range needs LO-HI (hex, HI exclusive)")
    return int(lo, 16), int(hi, 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--trials", type=int, default=8, help="trials per function")
    ap.add_argument("--only", help="these functions only (hex addresses, comma-separated)")
    ap.add_argument("--range", type=parse_hex_range, help="functions starting in LO-HI (hex, HI exclusive)")
    ap.add_argument("--first", type=int, help="only the first N lifted functions")
    ap.add_argument("--seed", type=int, default=750)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel workers (default: one per CPU)")
    ap.add_argument("--show", type=int, default=30, help="mismatching functions to print")
    args = ap.parse_args()

    lifted, not_lifted = functions(args.gen)
    if args.only:
        wanted = [int(x, 16) for x in args.only.split(",")]
        missing = [x for x in wanted if x not in set(lifted)]
        if missing:
            sys.exit(f"difftest: not lifted functions: {', '.join(f'{x:08X}' for x in missing)}")
        lifted = wanted
    if args.range:
        lifted = [fn for fn in lifted if args.range[0] <= fn < args.range[1]]
    if args.first:
        lifted = lifted[:args.first]
    if not lifted:
        sys.exit("difftest: no functions selected")
    jobs = max(1, min(args.jobs, len(lifted)))
    init = (args.harness, args.dol, not_lifted, args.trials, args.seed)

    counts, stops, bad = collections.Counter(), collections.Counter(), {}
    started = time.monotonic()

    def merge(results):
        for k, (fn, c, s, why) in enumerate(results):
            counts.update(c)
            stops.update(s)
            if why:
                bad[fn] = why
            if (k + 1) % 500 == 0:
                print(f"  {k + 1}/{len(lifted)} functions, {counts['MISMATCH']} mismatching trials", file=sys.stderr)

    if jobs == 1:  # in-process: plain tracebacks, easy to debug
        init_worker(*init)
        merge(map(test_function, lifted))
    else:
        with multiprocessing.Pool(jobs, init_worker, init) as pool:
            merge(pool.imap_unordered(test_function, lifted, chunksize=4))
    elapsed = time.monotonic() - started

    total = sum(counts.values())
    print(f"{len(lifted)} functions, {total} trials, {jobs} workers, {elapsed:.1f} s:")
    for key, n in sorted(counts.items()):
        print(f"  {n:7} {key}")
    print("  agreeing trials ended: " + ", ".join(f"{label}: {n}" for label, n in stops.most_common()))
    print(f"{len(bad)} functions with a mismatch")
    for fn in sorted(bad)[:args.show]:
        print(f"  {fn:08X} {bad[fn]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
