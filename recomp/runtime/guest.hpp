// What lifted code needs at run time: the guest's register context, its
// big-endian memory, and the arithmetic, condition-register and carry helpers
// the lifter's output calls. Header-only and runtime code: no std::format
// (see CMakeLists.txt on macOS 10.15).
//
// Bits are numbered here as in IBM's manuals, from 0 at the most significant:
// CR field n occupies bits 4n-4n+3 (LT, GT, EQ, SO), so CR0 is the top nibble.
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian; the guest is big-endian");

// XER bits.
inline constexpr uint32_t kXerSo = 0x80000000u, kXerOv = 0x40000000u, kXerCa = 0x20000000u;

struct Context {
    uint32_t r[32] = {};
    uint32_t lr = 0, ctr = 0, cr = 0, xer = 0;
    uint32_t ret = 0;            // where the last function returned to (its blr's target)
    uint32_t reserve = 0;        // lwarx reservation held
    uint32_t reserve_addr = 0;
    uint32_t spr[1024] = {};     // other special-purpose registers (GQRs, HID2, ...), by number
    double f[32] = {};           // FPRs; in paired-single mode, each one's ps0
    double ps1[32] = {};         // each FPR's ps1 (a single-precision value, held as a double)
};

// What stops lifted code: an invalid memory access, an unknown indirect target,
// a return somewhere unexpected, a trap, something not lifted. Thrown, so the
// runtime can name it and its caller, and the differential test can compare it.
struct Stop {
    // Unregistered: a replaced function the runtime has no native code for
    // yet (pc: the function, value: where it was called from). Idle: no
    // guest thread can run and nothing would make one ready (pc: the
    // scheduler, value: the thread that went to sleep last).
    enum Kind : uint32_t { Memory, BadJump, BadReturn, Trap, NotLifted, Unsupported, Unregistered, Idle } kind;
    uint32_t pc;     // guest address of the instruction (or function)
    uint32_t value;  // the address involved
};

// Guest memory: the 4 GB guest address space at a host base, `base + addr`.
inline uint8_t* g_mem = nullptr;

#ifdef QILIN_CHECKED_MEMORY
// Bring-up and tests: accesses outside the regions the host mapped stop.
struct Region {
    uint32_t lo, hi;
};
inline Region g_regions[16] = {};
inline unsigned g_region_count = 0;
inline uint32_t g_pc = 0;  // set by checked code before each access, for the report
// `ea` is the address the instruction used, reported if [a, a + n) isn't mapped.
inline void check(uint32_t a, uint32_t n, uint32_t ea) {
    for (unsigned i = 0; i < g_region_count; ++i)
        if (a >= g_regions[i].lo && uint64_t(a) + n <= g_regions[i].hi) return;
    throw Stop{Stop::Memory, g_pc, ea};
}
inline void check(uint32_t a, uint32_t n) { check(a, n, a); }
#define QILIN_PC(a) (::qilin::rt::g_pc = (a))
#else
inline void check(uint32_t, uint32_t, uint32_t) {}
inline void check(uint32_t, uint32_t) {}
#define QILIN_PC(a) ((void)0)
#endif

inline uint8_t ld8(uint32_t a) {
    check(a, 1);
    return g_mem[a];
}
inline uint16_t ld16(uint32_t a) {
    check(a, 2);
    uint16_t v;
    std::memcpy(&v, g_mem + a, 2);
    return std::byteswap(v);
}
inline uint32_t ld32(uint32_t a) {
    check(a, 4);
    uint32_t v;
    std::memcpy(&v, g_mem + a, 4);
    return std::byteswap(v);
}
inline void st8(uint32_t a, uint32_t v) {
    check(a, 1);
    g_mem[a] = static_cast<uint8_t>(v);
}
inline void st16(uint32_t a, uint32_t v) {
    check(a, 2);
    const uint16_t b = std::byteswap(static_cast<uint16_t>(v));
    std::memcpy(g_mem + a, &b, 2);
}
inline void st32(uint32_t a, uint32_t v) {
    check(a, 4);
    const uint32_t b = std::byteswap(v);
    std::memcpy(g_mem + a, &b, 4);
}
// Byte-reversed forms (lhbrx, lwbrx, sthbrx, stwbrx): little-endian in guest memory.
inline uint32_t ld16r(uint32_t a) { return std::byteswap(ld16(a)); }
inline uint32_t ld32r(uint32_t a) { return std::byteswap(ld32(a)); }
inline void st16r(uint32_t a, uint32_t v) { st16(a, std::byteswap(static_cast<uint16_t>(v))); }
inline void st32r(uint32_t a, uint32_t v) { st32(a, std::byteswap(v)); }
inline uint32_t lha(uint32_t a) { return static_cast<uint32_t>(static_cast<int16_t>(ld16(a))); }

// dcbz: zero the 32-byte block containing a.
inline void dcbz(uint32_t ea) {
    const uint32_t a = ea & ~31u;
    check(a, 32, ea);
    std::memset(g_mem + a, 0, 32);
}

// ---- condition register ----
inline constexpr uint32_t kLt = 8, kGt = 4, kEq = 2, kSo = 1;  // a field's bits, as a 4-bit value

inline uint32_t so_bit(uint32_t xer) { return xer >> 31; }
inline uint32_t cmps(uint32_t a, uint32_t b, uint32_t xer) {
    const auto x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    return (x < y ? kLt : x > y ? kGt : kEq) | so_bit(xer);
}
inline uint32_t cmpu(uint32_t a, uint32_t b, uint32_t xer) { return (a < b ? kLt : a > b ? kGt : kEq) | so_bit(xer); }
// Set CR field f (0-7) to a 4-bit value.
inline uint32_t set_crf(uint32_t cr, unsigned f, uint32_t v) {
    const unsigned shift = 28 - 4 * f;
    return (cr & ~(0xFu << shift)) | (v << shift);
}
// The record forms: CR0 from the signed result and SO.
inline uint32_t set_cr0(uint32_t cr, uint32_t result, uint32_t xer) { return set_crf(cr, 0, cmps(result, 0, xer)); }
inline uint32_t crb(uint32_t cr, unsigned b) { return (cr >> (31 - b)) & 1; }
inline uint32_t set_crb(uint32_t cr, unsigned b, uint32_t v) {
    return (cr & ~(1u << (31 - b))) | ((v & 1) << (31 - b));
}

// ---- XER ----
inline uint32_t ca(uint32_t xer) { return (xer >> 29) & 1; }
inline uint32_t set_ca(uint32_t xer, uint32_t c) { return (xer & ~kXerCa) | (c << 29); }
// OE forms: OV is set or cleared, SO is sticky.
inline uint32_t set_ov(uint32_t xer, bool ov) { return ov ? xer | kXerOv | kXerSo : xer & ~kXerOv; }

// a + b + c (c = 0 or 1), with carry out and signed overflow.
struct Sum {
    uint32_t v, carry;
    bool ov;
};
inline Sum add3(uint32_t a, uint32_t b, uint32_t c) {
    const uint64_t s = uint64_t(a) + b + c;
    const auto v = static_cast<uint32_t>(s);
    return {v, static_cast<uint32_t>(s >> 32), (((a ^ v) & (b ^ v)) >> 31) != 0};
}

// ---- multiply and divide (Broadway's results where the architecture leaves them undefined) ----
inline uint32_t mulhw(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((int64_t(static_cast<int32_t>(a)) * static_cast<int32_t>(b)) >> 32);
}
inline uint32_t mulhwu(uint32_t a, uint32_t b) { return static_cast<uint32_t>((uint64_t(a) * b) >> 32); }
inline bool mullw_ov(uint32_t a, uint32_t b) {
    const int64_t p = int64_t(static_cast<int32_t>(a)) * static_cast<int32_t>(b);
    return p < INT32_MIN || p > INT32_MAX;
}
inline bool divw_ov(uint32_t a, uint32_t b) { return b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu); }
// Divide by zero, or 0x80000000 / -1: all ones if the dividend is negative, else 0.
inline uint32_t divw(uint32_t a, uint32_t b) {
    if (divw_ov(a, b)) return static_cast<int32_t>(a) < 0 ? 0xFFFFFFFFu : 0;
    return static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

// ---- shifts ----
inline uint32_t slw(uint32_t s, uint32_t n) { return (n & 0x20) ? 0 : s << (n & 31); }
inline uint32_t srw(uint32_t s, uint32_t n) { return (n & 0x20) ? 0 : s >> (n & 31); }
// sraw/srawi: the result, and CA = 1 if the source is negative and 1-bits were shifted out.
inline Sum sraw(uint32_t s, uint32_t n) {
    const auto x = static_cast<int32_t>(s);
    if (n & 0x20) return {static_cast<uint32_t>(x >> 31), s >> 31, false};
    n &= 31;
    const uint32_t lost = n ? s & ((1u << n) - 1) : 0;
    return {static_cast<uint32_t>(x >> n), (x < 0 && lost) ? 1u : 0u, false};
}

// ---- string loads and stores with a count in XER (lswx, stswx) ----
inline void lswx(Context& c, unsigned rd, uint32_t ea) {
    const unsigned n = c.xer & 0x7F;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t& r = c.r[(rd + i / 4) % 32];
        if (i % 4 == 0) r = 0;
        r |= uint32_t(ld8(ea + i)) << (24 - 8 * (i % 4));
    }
}
inline void stswx(Context& c, unsigned rs, uint32_t ea) {
    const unsigned n = c.xer & 0x7F;
    for (unsigned i = 0; i < n; ++i) st8(ea + i, c.r[(rs + i / 4) % 32] >> (24 - 8 * (i % 4)));
}

// ---- floating point (slice 3) ----
// FPSCR isn't modelled: only the OS (replaced) and the debugger stub (dropped)
// read or write it. Lifted code runs in the mode OSInit sets and nothing
// changes: round to nearest, exceptions disabled, and non-IEEE mode
// (PPCSetFpNonIEEEMode, FPSCR[NI] = 1). The runtime that replaces the OS must
// stop on any other mode. In non-IEEE mode the 750 family treats denormal
// operands as zero and returns zero for denormal results, keeping the sign
// (IBM 750GX/GL user's manual, table 2-5). Single-precision instructions
// judge "denormal" by the single format, double-precision ones by the double.
//
// Where Broadway's exact result isn't pinned down by a source, lifted code
// stops (fp_unknown) instead of guessing: a single-precision multiply whose
// frC isn't a single (Broadway rounds frC to fewer bits first), and a compare
// of a denormal under NI.
inline uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
inline double dbl(uint64_t b) { return std::bit_cast<double>(b); }

inline uint64_t ld64(uint32_t a) {
    check(a, 8);
    uint64_t v;
    std::memcpy(&v, g_mem + a, 8);
    return std::byteswap(v);
}
inline void st64(uint32_t a, uint64_t v) {
    check(a, 8);
    const uint64_t b = std::byteswap(v);
    std::memcpy(g_mem + a, &b, 8);
}

[[noreturn]] inline void fp_unknown(uint32_t pc) { throw Stop{Stop::Unsupported, pc, 0xFFFFFFFFu}; }

#ifdef QILIN_CHECKED_MEMORY
inline uint32_t g_ni_flushes = 0;  // the differential test: Unicorn ignores NI, so a flush is a known difference
#define QILIN_NI_FLUSHED() (++::qilin::rt::g_ni_flushes)
#else
#define QILIN_NI_FLUSHED() ((void)0)
#endif

// The smallest normal double and single. Not DBL_MIN/FLT_MIN: zig's macOS
// headers lose those macros when <chrono> comes before <cfloat>. The
// parentheses keep <windows.h>'s min macro out (the difftest harness
// includes it first).
inline constexpr double kMinNormalD = (std::numeric_limits<double>::min)();
inline constexpr double kMinNormalS = (std::numeric_limits<float>::min)();

// Non-IEEE mode: a denormal (in the given format) becomes zero of its sign.
inline double ni_d(double x) {
    if (std::fabs(x) < kMinNormalD && x != 0.0) [[unlikely]] {
        QILIN_NI_FLUSHED();
        return std::copysign(0.0, x);
    }
    return x;
}
inline double ni_s(double x) {
    if (std::fabs(x) < kMinNormalS && x != 0.0) [[unlikely]] {
        QILIN_NI_FLUSHED();
        return std::copysign(0.0, x);
    }
    return x;
}
inline bool denormal_d(double x) { return std::fabs(x) < kMinNormalD && x != 0.0; }
// frC of a single-precision multiply: exactly a single's 24-bit significand.
inline bool single_significand(double x) { return (bits(x) & 0x1FFFFFFFu) == 0; }

// NaN results, as PowerPC chooses them (host CPUs choose differently): the
// first NaN operand in the order frA, frB, frC, made quiet; else (an invalid
// operation) the default QNaN, positive.
inline double quiet(double x) { return dbl(bits(x) | 0x0008000000000000ull); }
inline double nan_of(double a, double b, double c) {
    if (std::isnan(a)) return quiet(a);
    if (std::isnan(b)) return quiet(b);
    if (std::isnan(c)) return quiet(c);
    return dbl(0x7FF8000000000000ull);
}
// A NaN rounded to single precision keeps the top 23 fraction bits.
inline double single_nan(double x) { return dbl(bits(x) & ~0x1FFFFFFFull); }
// A double result rounded to single precision (double rounding, as Broadway
// computes single-precision instructions), then NI.
inline double to_single(double r) { return ni_s(static_cast<double>(static_cast<float>(r))); }

// Double-precision arithmetic. Operand order is frA, frB, frC for NaNs.
inline double fadd(double a, double b) {
    const double r = ni_d(a) + ni_d(b);
    return std::isnan(r) ? nan_of(a, b, 0.0) : ni_d(r);
}
inline double fsub(double a, double b) {
    const double r = ni_d(a) - ni_d(b);
    return std::isnan(r) ? nan_of(a, b, 0.0) : ni_d(r);
}
inline double fmul(double a, double c) {
    const double r = ni_d(a) * ni_d(c);
    return std::isnan(r) ? nan_of(a, c, 0.0) : ni_d(r);
}
inline double fdiv(double a, double b) {
    const double r = ni_d(a) / ni_d(b);
    return std::isnan(r) ? nan_of(a, b, 0.0) : ni_d(r);
}
// Fused multiply-add (one rounding): a*c + b, a*c - b, and their negations.
// The negated forms leave a NaN's sign alone.
inline double fma_(double a, double c, double b, bool sub, bool neg) {
    const double r = std::fma(ni_d(a), ni_d(c), sub ? -ni_d(b) : ni_d(b));
    if (std::isnan(r)) return nan_of(a, b, c);
    return ni_d(neg ? -r : r);
}

// Single precision: operands flushed by the single format, the double result
// rounded to single.
inline double fadds(double a, double b) {
    const double r = ni_s(a) + ni_s(b);
    return std::isnan(r) ? single_nan(nan_of(a, b, 0.0)) : to_single(r);
}
inline double fsubs(double a, double b) {
    const double r = ni_s(a) - ni_s(b);
    return std::isnan(r) ? single_nan(nan_of(a, b, 0.0)) : to_single(r);
}
inline double fmuls(double a, double c) {
    const double r = ni_s(a) * ni_s(c);
    return std::isnan(r) ? single_nan(nan_of(a, c, 0.0)) : to_single(r);
}
inline double fdivs(double a, double b) {
    const double r = ni_s(a) / ni_s(b);
    return std::isnan(r) ? single_nan(nan_of(a, b, 0.0)) : to_single(r);
}
inline double fmas(double a, double c, double b, bool sub, bool neg) {
    const double r = std::fma(ni_s(a), ni_s(c), sub ? -ni_s(b) : ni_s(b));
    if (std::isnan(r)) return single_nan(nan_of(a, b, c));
    return to_single(neg ? -r : r);
}
inline double frsp(double b) {
    const double x = ni_d(b);
    return std::isnan(x) ? single_nan(quiet(x)) : to_single(x);
}

// Moves: bit operations, NaNs included.
inline double fneg(double x) { return dbl(bits(x) ^ 0x8000000000000000ull); }
inline double fabs_(double x) { return dbl(bits(x) & ~0x8000000000000000ull); }

// fctiwz: truncate to a signed word, saturating; NaN gives 0x80000000. The
// high word is undefined in the architecture; the 750 family's is 0xFFF80000.
inline double fctiwz(double b) {
    uint32_t v;
    if (std::isnan(b)) v = 0x80000000u;
    else if (b >= 2147483648.0) v = 0x7FFFFFFFu;
    else if (b <= -2147483649.0) v = 0x80000000u;
    else v = static_cast<uint32_t>(static_cast<int32_t>(b));
    return dbl(0xFFF8000000000000ull | v);
}

// The estimates (frsqrte, fres): Broadway reads them from tables no source
// here pins down (the owner has no console to measure them on). The
// architecture defines the rest exactly: a NaN stays a NaN (quietened), a
// zero gives an infinity of its sign, an infinity gives a zero, and a
// negative number's square root is the default NaN. Under non-IEEE mode a
// denormal input counts as a zero of its sign.
//
// Where the table is needed: the differential test stops (fp_unknown), so it
// never compares a guess. The game's runtime build defines
// QILIN_APPROX_ESTIMATES (the owner's decision, 2026-11-19): APPROX, the
// exact result rounded to 12 fraction bits, within the 1/4096 relative
// accuracy IBM's 750CL manual states for both. Callers refine with Newton
// steps, so results are right but for the last bits.
inline double estimate_12(double r) {  // round to nearest at 12 fraction bits
    constexpr uint64_t kDrop = 52 - 12;
    return dbl((bits(r) + (1ull << (kDrop - 1))) & ~((1ull << kDrop) - 1));
}
inline double frsqrte(double b, [[maybe_unused]] uint32_t pc) {
    if (std::isnan(b)) return quiet(b);
    const double x = ni_d(b);
    if (x == 0.0) return std::copysign(HUGE_VAL, x);
    if (x < 0.0) return dbl(0x7FF8000000000000ull);
    if (std::isinf(x)) return 0.0;
#ifdef QILIN_APPROX_ESTIMATES
    return estimate_12(1.0 / std::sqrt(x));
#else
    fp_unknown(pc);
#endif
}
inline double fres(double b, [[maybe_unused]] uint32_t pc) {  // single precision
    if (std::isnan(b)) return single_nan(quiet(b));
    const double x = ni_s(b);
    if (x == 0.0) return std::copysign(HUGE_VAL, x);
    if (std::isinf(x)) return std::copysign(0.0, x);
#ifdef QILIN_APPROX_ESTIMATES
    return ni_s(to_single(estimate_12(1.0 / x)));
#else
    fp_unknown(pc);
#endif
}

// fcmpu/fcmpo: the CR field (FL, FG, FE, FU). FPSCR's FPCC and VX bits aren't kept.
inline uint32_t fcmp(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return 1;
    return a < b ? kLt : a > b ? kGt : kEq;
}

// lfs: single to double, exactly; NaNs keep their payload (and stay
// signalling), which a host conversion wouldn't promise.
inline double lfs(uint32_t w) {
    if ((w & 0x7F800000u) == 0x7F800000u)
        return dbl(uint64_t(w & 0x80000000u) << 32 | 0x7FF0000000000000ull | uint64_t(w & 0x007FFFFFu) << 29);
    return static_cast<double>(std::bit_cast<float>(w));
}
// stfs: double to single by the architecture's bit copy (no rounding), with
// denormalisation for values below the single range. Smaller values, where
// the architecture leaves the result undefined, become zero of their sign.
inline uint32_t stfs(double d) {
    const uint64_t x = bits(d);
    const unsigned e = static_cast<unsigned>(x >> 52) & 0x7FF;
    if (e > 896 || (x & 0x7FFFFFFFFFFFFFFFull) == 0)
        return static_cast<uint32_t>((x >> 32) & 0xC0000000u) | static_cast<uint32_t>((x >> 29) & 0x3FFFFFFFu);
    const uint32_t sign = static_cast<uint32_t>(x >> 32) & 0x80000000u;
    if (e < 874) return sign;
    const uint64_t frac = 0x8000000000000000ull | (x << 11);  // 1.f, left-aligned
    return sign | static_cast<uint32_t>((frac >> (897 - e)) >> 40);
}

// ---- paired singles (slice 4) ----
// Lifted code runs in paired-single mode (HID2[PSE] = 1, set by the OS at
// boot; the replacing runtime must keep it so). Each FPR holds a pair: ps0 is
// the FPR as double-precision instructions see it, ps1 a second value. From
// the 750CL user's manual: single-precision arithmetic, frsp and lfs write
// their result to both halves; double-precision instructions and fmr write ps0
// and leave ps1 alone (CodeWarrior's prologues and epilogues rely on this when
// they save an FPR with stfd and psq_st and restore it with psq_l then lfd);
// stfs stores ps0. The manual leaves ps1 after frsp undefined: it gets the
// result, as for the other single-precision instructions.
//
// psq_l/psq_st convert through a GQR (SPR 912-919): a type (0 single,
// 4 u8, 5 u16, 6 s8, 7 s16; 1-3 reserved) and a signed 6-bit scale S, for
// loads and stores separately. A load gives I * 2^-S; a store gives
// I * 2^S rounded toward zero and clamped to the type, where +inf and NaNs
// with sign 0 clamp high, -inf and NaNs with sign 1 low. A single passes
// through unconverted, except that a store of a denormal stores 0.0.
inline unsigned gqr_ld_type(uint32_t g) { return (g >> 16) & 7; }
inline int gqr_ld_scale(uint32_t g) { return static_cast<int32_t>(g << 2) >> 26; }
inline unsigned gqr_st_type(uint32_t g) { return g & 7; }
inline int gqr_st_scale(uint32_t g) { return static_cast<int32_t>(g << 18) >> 26; }
inline uint32_t quant_size(unsigned type) { return type == 0 ? 4 : (type & 1) ? 2 : 1; }

inline double dequantize(uint32_t ea, unsigned type, int scale) {
    switch (type) {
    case 4: return std::ldexp(static_cast<double>(ld8(ea)), -scale);
    case 5: return std::ldexp(static_cast<double>(ld16(ea)), -scale);
    case 6: return std::ldexp(static_cast<double>(static_cast<int8_t>(ld8(ea))), -scale);
    case 7: return std::ldexp(static_cast<double>(static_cast<int16_t>(ld16(ea))), -scale);
    default: return lfs(ld32(ea));
    }
}
void fifo_st8(uint32_t ea, uint32_t v);
void fifo_st16(uint32_t ea, uint32_t v);
void fifo_st32(uint32_t ea, uint32_t v);

// Fifo: the store goes to the GX write-gather pipe (psq_st there, which the
// GX library's matrix loads do), not to memory.
template <bool Fifo = false>
inline void quantize(uint32_t ea, double v, unsigned type, int scale) {
    if (type == 0) {
        uint32_t w = stfs(v);
        if ((w & 0x7F800000u) == 0 && (bits(v) & 0x7FFFFFFFFFFFFFFFull) != 0) w = 0;  // a denormal stores 0.0
        if constexpr (Fifo) fifo_st32(ea, w);
        else st32(ea, w);
        return;
    }
    const double lo = type == 6 ? -128.0 : type == 7 ? -32768.0 : 0.0;
    const double hi = type == 4 ? 255.0 : type == 5 ? 65535.0 : type == 6 ? 127.0 : 32767.0;
    double x;
    if (std::isnan(v)) x = std::signbit(v) ? lo : hi;
    else {
        x = std::trunc(std::ldexp(v, scale));
        x = x < lo ? lo : x > hi ? hi : x;
    }
    const auto i = static_cast<uint32_t>(static_cast<int32_t>(x));
    if constexpr (Fifo) {
        if (type & 1) fifo_st16(ea, i);
        else fifo_st8(ea, i);
    } else {
        if (type & 1) st16(ea, i);
        else st8(ea, i);
    }
}
// psq_l: W = 1 loads ps0 only, and ps1 becomes 1.0. Both loads happen before
// either half is written, so a fault leaves frD as it was. It writes lifted
// code's locals through references: the lifter's register dataflow knows
// (writes() in lifter.cpp); any new helper like it must be added there.
inline void psq_l(double& p0, double& p1, uint32_t ea, bool w, uint32_t gqr, uint32_t pc) {
    const unsigned t = gqr_ld_type(gqr);
    if (t >= 1 && t <= 3) fp_unknown(pc);  // reserved type
    const int s = gqr_ld_scale(gqr);
    const double a = dequantize(ea, t, s);
    const double b = w ? 1.0 : dequantize(ea + quant_size(t), t, s);
    p0 = a;
    p1 = b;
}
template <bool Fifo = false>
inline void psq_st(double p0, double p1, uint32_t ea, bool w, uint32_t gqr, uint32_t pc) {
    const unsigned t = gqr_st_type(gqr);
    if (t >= 1 && t <= 3) fp_unknown(pc);
    const int s = gqr_st_scale(gqr);
    quantize<Fifo>(ea, p0, t, s);
    if (!w) quantize<Fifo>(ea + quant_size(t), p1, t, s);
}
inline void fifo_psq_st(double p0, double p1, uint32_t ea, bool w, uint32_t gqr, uint32_t pc) {
    psq_st<true>(p0, p1, ea, w, gqr, pc);
}

#ifdef QILIN_DETERMINISTIC_TIME
// The differential test only: the k-th time-base read since the counter was
// reset gives kTimeStart + k * kTimeStep, as the test's Unicorn side does, so
// trials that read the time base are compared and replay exactly.
inline uint64_t g_time_reads = 0;
inline constexpr uint64_t kTimeStart = 0x0000000100000000ull, kTimeStep = 0x1000;
#endif

// ---- safe points in lifted loops ----
// The lifter puts a poll on every backward branch, so a loop that waits for
// an interrupt (a flag a callback sets) gets it, as on the console: when an
// interrupt is queued, and every 1024th time round for the timers. Only one
// guest thread runs at a time, so the counter needs no atomics.
inline std::atomic<bool> g_irq_pending{false};
inline uint32_t g_poll_tick = 0;
inline bool poll_due() {
    return g_irq_pending.load(std::memory_order_relaxed) || (++g_poll_tick & 1023u) == 0;
}

// ---- provided by the runtime (dispatch.cpp and the host) ----
// A safe point in a lifted loop: delivers what is due (the runtime), or
// nothing (the differential test). The lifter stores the loop's stack
// pointer first, so handlers build their frames below it.
void poll(Context& c);
// dcbf and dcbst: a cache line written back to memory, which the GPU may
// now read (the runtime records it for the renderer's caches,
// coherence.hpp; the differential test does nothing).
void cache_store(uint32_t ea);
// Call the lifted function at a guest address (bctrl, bctr, blrl). Unknown
// targets stop with BadJump.
void call(Context& c, uint32_t target, uint32_t from);
uint32_t mfspr(Context& c, unsigned spr, uint32_t pc);
void mtspr(Context& c, unsigned spr, uint32_t v, uint32_t pc);
uint32_t mftb(Context& c, unsigned tbr);
// The 64-bit time base mftb reads (kTimeBaseHz ticks a second, from the
// host's steady clock).
inline constexpr uint64_t kTimeBaseHz = 60'750'000, kNsPerSecond = 1'000'000'000;
uint64_t time_base();
// The host clock in the same ticks, the game's pauses included, and adding
// to the time the game doesn't see pass (the settings menu: pause.cpp).
uint64_t time_base_unpaused();
void skip_time_base(uint64_t ticks);
// The game's clock may run a little faster or slower than the host's (the
// display lock: host.cpp), by `speed`. Set once, before the game's threads
// start; the clock carries on from where it was. host_ns_for_ticks converts
// a span of the game's ticks to host nanoseconds at that speed.
void set_time_base_speed(double speed);
double time_base_speed();
uint64_t host_ns_for_ticks(uint64_t ticks);
void syscall(Context& c, uint32_t pc);
// fifo_st8/16/32 (declared above psq_st): stores to the GX write-gather pipe
// (0xCC008000). The lifter emits them for the stores hwaccess finds there
// (the SDK's inline vertex functions, compiled into lifted code). The
// runtime decodes the command stream; the differential test stops on them as
// it does on any address it doesn't map.
// `qilin lift --runtime` only: a function of a replaced library, by its index
// in the generated g_natives table. Runs the native code registered for it
// (native.hpp) and returns to the caller as a blr would; stops with
// Unregistered if there is none.
void native(Context& c, uint32_t index);

[[noreturn]] inline void trap(uint32_t pc) { throw Stop{Stop::Trap, pc, 0}; }
[[noreturn]] inline void bad_jump(uint32_t pc, uint32_t target) { throw Stop{Stop::BadJump, pc, target}; }
[[noreturn]] inline void bad_return(uint32_t pc, uint32_t to) { throw Stop{Stop::BadReturn, pc, to}; }
[[noreturn]] inline void not_lifted(uint32_t fn) { throw Stop{Stop::NotLifted, fn, 0}; }

using Fn = void (*)(Context&);
struct FnEntry {
    uint32_t addr;
    Fn fn;
};

}  // namespace qilin::rt
