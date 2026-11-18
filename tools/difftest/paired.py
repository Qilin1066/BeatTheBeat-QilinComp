"""Paired singles for the differential test: a reference model of Broadway's
paired-single state and instructions, which Unicorn doesn't have.

Written from the IBM 750CL user's manual, independently of the lifted code's
helpers (recomp/runtime/guest.hpp): the same specification, different code.
Unicorn keeps ps0 (the FPRs); this model keeps ps1 and the GQRs, and:

- emulates every paired-single instruction (ps_*, psq_*) in a code hook,
  then skips it;
- after an instruction that writes both halves in paired-single mode (lfs
  and its forms, single-precision arithmetic, frsp), copies ps0's result to
  ps1;
- emulates mtspr/mfspr of the GQRs (SPRs 912-919).

Arithmetic is IEEE (no non-IEEE flushing), as Unicorn's: a trial in which the
lifted code flushed a denormal is a known difference anyway. Each lane rounds
to single precision as the single-precision instruction of the same name
does, and NaNs follow PowerPC's rules (the first NaN of frA, frB, frC,
quieted; else the default QNaN 0x7FF8000000000000).
"""
import ctypes
import math
import struct
from fractions import Fraction

QNAN = 0x7FF8000000000000
SIGN = 0x8000000000000000
MEM1, MEM1_SIZE = 0x80000000, 24 << 20


def b2d(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def d2b(d):
    return struct.unpack("<Q", struct.pack("<d", d))[0]


def is_nan(b):
    return (b >> 52) & 0x7FF == 0x7FF and b & 0xFFFFFFFFFFFFF


def nan_of(*operands):
    for b in operands:
        if is_nan(b):
            return b | 0x0008000000000000
    return QNAN


def to_single(b):
    """A double's bits rounded to single precision, as bits of a double."""
    if is_nan(b):
        return b & ~0x1FFFFFFF
    return d2b(ctypes.c_float(b2d(b)).value)


def add(a, b, sub=False):
    if is_nan(a) or is_nan(b):
        return nan_of(a, b)
    x, y = b2d(a), b2d(b)
    if sub:
        y = -y
    if math.isinf(x) and math.isinf(y) and (x > 0) != (y > 0):
        return QNAN
    return d2b(x + y)


def mul(a, c):
    if is_nan(a) or is_nan(c):
        return nan_of(a, c)
    x, y = b2d(a), b2d(c)
    if (math.isinf(x) and y == 0) or (math.isinf(y) and x == 0):
        return QNAN
    return d2b(x * y)


def div(a, b):
    if is_nan(a) or is_nan(b):
        return nan_of(a, b)
    x, y = b2d(a), b2d(b)
    if (x == 0 and y == 0) or (math.isinf(x) and math.isinf(y)):
        return QNAN
    if y == 0:
        return d2b(math.copysign(math.inf, x) * math.copysign(1.0, y))
    return d2b(x / y)


def fma(a, c, b, sub, neg):
    """a*c + b (or - b), one rounding to double, then negated if asked."""
    if is_nan(a) or is_nan(b) or is_nan(c):
        return nan_of(a, b, c)
    x, y, z = b2d(a), b2d(c), b2d(b)
    if sub:
        z = -z
    if (math.isinf(x) and y == 0) or (math.isinf(y) and x == 0):
        return QNAN
    if math.isinf(x) or math.isinf(y):
        p = math.copysign(math.inf, x) * math.copysign(1.0, y)
        if math.isinf(z) and (z > 0) != (p > 0):
            return QNAN
        r = p
    elif math.isinf(z):
        r = z
    else:
        exact = Fraction(x) * Fraction(y) + Fraction(z)
        if exact == 0:
            psign = math.copysign(1.0, x) * math.copysign(1.0, y) < 0
            zsign = math.copysign(1.0, z) < 0
            r = -0.0 if (x * y == 0 and z == 0 and psign and zsign) else 0.0
        else:
            try:
                r = float(exact)
            except OverflowError:
                r = math.copysign(math.inf, exact)
    if neg:
        r = -r
    return d2b(r)


# ---- quantisation (GQR types: 0 single, 4 u8, 5 u16, 6 s8, 7 s16) ----
LIMITS = {4: (0, 255), 5: (0, 65535), 6: (-128, 127), 7: (-32768, 32767)}


def scale_of(six):
    return six - 64 if six & 0x20 else six


def single_to_double_bits(w):
    """lfs's conversion: exact, NaN payloads kept."""
    if w & 0x7F800000 == 0x7F800000:
        return (w & 0x80000000) << 32 | 0x7FF0000000000000 | (w & 0x7FFFFF) << 29
    return d2b(struct.unpack(">f", struct.pack(">I", w))[0])


def double_to_single_bits(b):
    """stfs's conversion: the architecture's bit copy, denormalising below the single range."""
    e = (b >> 52) & 0x7FF
    if e > 896 or b & ~SIGN == 0:
        return (b >> 32) & 0xC0000000 | (b >> 29) & 0x3FFFFFFF
    sign = (b >> 32) & 0x80000000
    if e < 874:
        return sign
    frac = (1 << 63) | (b << 11) & ((1 << 63) - 1)
    return sign | (frac >> (897 - e)) >> 40


class Paired:
    def __init__(self, mu, P, read, write):
        self.mu, self.P = mu, P
        self.read, self.write = read, write  # memory access that report a fault (None) for unmapped addresses
        self.ps1 = [0] * 32
        self.gqr = [0] * 8

    def fpr(self, n):
        return self.mu.reg_read(self.P.UC_PPC_REG_FPR0 + n)

    def set_pair(self, n, p0, p1):
        self.mu.reg_write(self.P.UC_PPC_REG_FPR0 + n, p0)
        self.ps1[n] = p1

    def gpr(self, n):
        return self.mu.reg_read(self.P.UC_PPC_REG_0 + n)

    # -- quantised loads and stores; None means a memory fault at the returned address
    def load_one(self, ea, t, s):
        size = 4 if t == 0 else 2 if t & 1 else 1
        raw = self.read(ea, size)
        if raw is None:
            return None
        if t == 0:
            return single_to_double_bits(struct.unpack(">I", raw)[0])
        i = int.from_bytes(raw, "big", signed=t in (6, 7))
        return d2b(math.ldexp(float(i), -s))

    def store_one(self, ea, b, t, s):
        if t == 0:
            w = double_to_single_bits(b)
            if w & 0x7F800000 == 0 and b & ~SIGN:
                w = 0  # a denormal stores 0.0
            return self.write(ea, struct.pack(">I", w))
        lo, hi = LIMITS[t]
        if is_nan(b):
            i = lo if b & SIGN else hi
        else:
            try:
                x = math.ldexp(b2d(b), s)
            except OverflowError:  # C's ldexp gives an infinity; Python's raises
                x = math.copysign(math.inf, b2d(b))
            i = hi if x >= hi else lo if x <= lo else int(x)  # int() truncates toward zero
        return self.write(ea, i.to_bytes(2 if t & 1 else 1, "big", signed=t in (6, 7)))

    def execute(self, pc, w):
        """Emulate the paired-single instruction w at pc. Returns None, or a
        (stop kind, pc, value) the trial ends with, in the harness's terms."""
        p, d, a, b, c = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if p in (56, 57, 60, 61) or (p == 4 and (w >> 1) & 0x3F in (6, 7, 38, 39)):
            if p == 4:
                xo = (w >> 1) & 0x3F
                load, update = xo in (6, 38), xo in (38, 39)
                wbit, i = (w >> 10) & 1, (w >> 7) & 7
                ea = ((self.gpr(a) if a else 0) + self.gpr(b)) & 0xFFFFFFFF
            else:
                load, update = p in (56, 57), p in (57, 61)
                wbit, i = (w >> 15) & 1, (w >> 12) & 7
                disp = w & 0xFFF
                disp = disp - 0x1000 if disp & 0x800 else disp
                ea = ((self.gpr(a) if a else 0) + disp) & 0xFFFFFFFF
            g = self.gqr[i]
            t = (g >> 16) & 7 if load else g & 7
            s = scale_of((g >> 24) & 0x3F if load else (g >> 8) & 0x3F)
            if 1 <= t <= 3:
                return (6, pc, 0xFFFFFFFF)  # reserved type: the lifted code stops too
            size = 4 if t == 0 else 2 if t & 1 else 1
            if load:
                x0 = self.load_one(ea, t, s)
                if x0 is None:
                    return (1, pc, ea)
                if wbit:
                    x1 = d2b(1.0)
                else:
                    x1 = self.load_one(ea + size, t, s)
                    if x1 is None:
                        return (1, pc, ea + size)
                self.set_pair(d, x0, x1)
            else:
                if not self.store_one(ea, self.fpr(d), t, s):
                    return (1, pc, ea)
                if not wbit and not self.store_one(ea + size, self.ps1[d], t, s):
                    return (1, pc, ea + size)
            if update:
                self.mu.reg_write(self.P.UC_PPC_REG_0 + a, ea)
            return None

        A0, A1 = self.fpr(a), self.ps1[a]
        B0, B1 = self.fpr(b), self.ps1[b]
        C0, C1 = self.fpr(c), self.ps1[c]
        x10 = (w >> 1) & 0x3FF
        x5 = (w >> 1) & 0x1F
        if x10 in (0, 32, 64, 96):  # ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1
            x, y = (A0, B0) if x10 in (0, 32) else (A1, B1)
            if self.denormal(x) or self.denormal(y):
                return (6, pc, 0xFFFFFFFF)
            if is_nan(x) or is_nan(y):
                f = 1
            else:
                f = 8 if b2d(x) < b2d(y) else 4 if b2d(x) > b2d(y) else 2
            crf = (w >> 23) & 7
            cr = self.mu.reg_read(self.P.UC_PPC_REG_CR)
            shift = 28 - 4 * crf
            self.mu.reg_write(self.P.UC_PPC_REG_CR, cr & ~(0xF << shift) | f << shift)
            return None
        moves = {40: lambda v: v ^ SIGN, 72: lambda v: v, 136: lambda v: v | SIGN, 264: lambda v: v & ~SIGN}
        if x10 in moves:  # ps_neg ps_mr ps_nabs ps_abs
            self.set_pair(d, moves[x10](B0), moves[x10](B1))
            return None
        merges = {528: (A0, B0), 560: (A0, B1), 592: (A1, B0), 624: (A1, B1)}
        if x10 in merges:
            self.set_pair(d, *merges[x10])
            return None
        s = to_single
        if x5 in (25, 12, 13, 29, 28, 31, 30, 14, 15):  # multiplies: frC lanes must be singles (else lifted stops)
            used = {25: (C0, C1), 12: (C0,), 13: (C1,), 14: (C0,), 15: (C1,)}.get(x5, (C0, C1))
            if any(v & 0x1FFFFFFF for v in used):
                return (6, pc, 0xFFFFFFFF)
        if x5 == 21:
            r = (s(add(A0, B0)), s(add(A1, B1)))
        elif x5 == 20:
            r = (s(add(A0, B0, True)), s(add(A1, B1, True)))
        elif x5 == 18:
            r = (s(div(A0, B0)), s(div(A1, B1)))
        elif x5 == 25:
            r = (s(mul(A0, C0)), s(mul(A1, C1)))
        elif x5 == 12:
            r = (s(mul(A0, C0)), s(mul(A1, C0)))
        elif x5 == 13:
            r = (s(mul(A0, C1)), s(mul(A1, C1)))
        elif x5 in (29, 28, 31, 30):  # madd msub nmadd nmsub
            sub, neg = x5 in (28, 30), x5 in (31, 30)
            r = (s(fma(A0, C0, B0, sub, neg)), s(fma(A1, C1, B1, sub, neg)))
        elif x5 == 14:
            r = (s(fma(A0, C0, B0, False, False)), s(fma(A1, C0, B1, False, False)))
        elif x5 == 15:
            r = (s(fma(A0, C1, B0, False, False)), s(fma(A1, C1, B1, False, False)))
        elif x5 == 10:  # ps_sum0
            r = (s(add(A0, B1)), C1)
        elif x5 == 11:  # ps_sum1
            r = (C0, s(add(A0, B1)))
        else:
            return (6, pc, 0xFFFFFFFF)  # not lifted either (ps_sel, estimates, dcbz_l)
        self.set_pair(d, *r)
        return None

    @staticmethod
    def denormal(b):
        return (b >> 52) & 0x7FF == 0 and b & ~SIGN


def classify(w):
    """What the paired-single model does at an instruction: 'emulate' (a
    paired-single instruction, or mtspr/mfspr of a GQR), 'dup' (writes both
    halves: copy ps0 to ps1 after it), or None."""
    p = w >> 26
    if p in (56, 57, 60, 61) or p == 4:
        return "emulate"
    if p in (48, 49) or (p == 31 and (w >> 1) & 0x3FF in (535, 567)):  # lfs lfsu lfsx lfsux
        return "dup"
    if p == 59 and (w >> 1) & 0x1F in (18, 20, 21, 24, 25, 28, 29, 30, 31):  # single-precision arithmetic, fres
        return "dup"
    if p == 63 and (w >> 1) & 0x3FF == 12:  # frsp
        return "dup"
    if p == 31 and (w >> 1) & 0x3FF in (339, 467):  # mfspr, mtspr
        spr = ((w >> 16) & 31) | ((w >> 11) & 31) << 5
        if 912 <= spr <= 919:
            return "emulate"
    return None


def execute_spr(model, pc, w):
    """mtspr/mfspr of a GQR, against the model's GQRs."""
    rd = (w >> 21) & 31
    spr = ((w >> 16) & 31) | ((w >> 11) & 31) << 5
    if (w >> 1) & 0x3FF == 467:
        model.gqr[spr - 912] = model.gpr(rd)
    else:
        model.mu.reg_write(model.P.UC_PPC_REG_0 + rd, model.gqr[spr - 912])
    return None
