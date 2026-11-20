// The registry of native functions: the runtime's replacements for the SDK
// libraries that drive hardware (docs/code-analysis.md). `qilin lift
// --runtime` turns every function of a replaced or dropped library into
// `rt::native(c, index)`; native code registers itself by the function's name
// (the decomp's), and bind_natives() joins the two at start-up. A replaced
// function nobody registered stops the game the moment it is called, naming
// it and its caller. Runtime code: no std::format.
//
//   QILIN_NATIVE(OSGetTick) { c.r[3] = ...; }
//
// A native function reads its arguments from the context as the EABI passes
// them (r3-r10, f1-f8) and leaves its result in r3 (r3:r4 for 64 bits) or f1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "guest.hpp"

namespace qilin::rt {

// Generated (natives.cpp): the replaced and dropped functions, by address.
struct NativeEntry {
    uint32_t addr;
    const char* name;     // the decomp's name, or fn_XXXXXXXX if it has none
    const char* library;  // "os", "gx", ...
};
extern const NativeEntry g_natives[];
extern const size_t g_native_count;
// Called before every native while set (it throws Stop to refuse one): GX
// sets it while a display list is recorded (gx_fifo.cpp).
using NativeGuard = void (*)(Context& c, const NativeEntry& e);
extern NativeGuard g_native_guard;

// Generated (symbols.cpp): every named function, by address. For reports,
// and for the few guest functions the runtime calls by name (main).
struct Symbol {
    uint32_t addr;
    const char* name;
};
extern const Symbol g_symbols[];
extern const size_t g_symbol_count;

// The function starting at `addr`, or the one `addr` is inside (the last
// symbol at or below it); nullptr if there is none.
const Symbol* symbol_at(uint32_t addr);
const Symbol* symbol_containing(uint32_t addr);
// The address of a function by name; 0 if the game has none (or several) of
// that name.
uint32_t symbol_address(const char* name);

class NativeRegistration {
public:
    // `code`: the first instructions the function must have (up to 4), for
    // natives whose name alone could be put on the wrong function (runs of
    // equal-size accessors, where the ported names were once three
    // functions out). Empty: no check.
    NativeRegistration(const char* name, Fn fn, std::initializer_list<uint32_t> code = {});
};

// The address of a lifted function native code calls by name. A missing or
// ambiguous name ends the run with a message: the runtime and the lift
// don't fit together.
uint32_t require_function(const char* name);
// Calls a lifted function from native code with the registers in `c`,
// keeping the native caller's link register (as a `bl` would, from the
// caller's point of view).
void call_guest(Context& c, uint32_t fn);

// What stopped the game, with function names, on stderr: also the guest call
// stack (from the stack frames' back chain and saved link registers) and
// the argument registers, as the running host thread's context last held
// them (lifted code syncs them at calls, so they are a call or so old).
void report_stop(const Stop& s);
// The context each host thread runs its guest thread in, for report_stop.
void set_thread_context(const Context* c);
// A guest call stack from a context's stack pointer, on stderr.
void report_stack(const Context& c);

// What the runtime has done so far, printed with a stop: shows which parts
// a run has exercised. Each counter is bumped by the running guest thread.
struct Stats {
    uint64_t switches = 0, threads_started = 0;
    uint64_t interrupts = 0, decrementer = 0;
    uint64_t files_opened = 0, bytes_read = 0;
    uint64_t draws = 0, vertices = 0, display_copies = 0, texture_copies = 0, retraces = 0;
    uint64_t audio_frames = 0;
    uint64_t dsp_frames = 0;
    uint64_t dvd_pending = 0;  // asynchronous disc requests not yet completed (now)  // AX frames the DSP mixed: one per audio frame on the console
    // Periodic interrupts that were late by a whole period or more and so
    // never ran (an audio frame lost is music running slow), and where
    // deliveries happened: in the idle scheduler, or at a safe point in
    // running code.
    uint64_t periods_dropped = 0, idle_deliveries = 0, running_deliveries = 0;
    uint64_t remotes_connected = 0, remote_reports = 0, voice_frames = 0, audio_frames_dropped = 0;
    uint64_t textures_uploaded = 0, frames_presented = 0;
    uint64_t draw_batches = 0;  // the renderer's vkCmdDraws: consecutive draws alike become one
    // The audio output's queue (host.cpp): how often it ran dry, and its
    // depth when each frame was queued (sum and count for the mean, in bytes
    // of 32 kHz stereo: 128 a millisecond).
    uint64_t audio_underruns = 0, audio_queue_sum = 0, audio_queue_count = 0;
    uint64_t audio_queue_min = ~uint64_t(0), audio_queue_max = 0;
    double audio_ratio = 1.0;  // the output's last resampling ratio
};
Stats& stats();
void report_stats();
// More for the report from parts not in every build (the host: the vblank
// lock); null if none.
inline std::string (*g_report_extra)() = nullptr;

// Joins registrations to g_natives. False, with the reason on stderr, if a
// registered name isn't a replaced function of this game, is ambiguous, is
// registered twice, or its code isn't what the registration expects: a
// replacement that can never run, or runs in the wrong place, is a mistake.
// Guest memory must hold the DOL if any registration states code.
bool bind_natives();
// How many of the game's replaced functions have native code.
size_t natives_bound();

#define QILIN_NATIVE(name)                                                        \
    static void qilin_native_##name(::qilin::rt::Context& c);                     \
    static const ::qilin::rt::NativeRegistration qilin_reg_##name{#name, qilin_native_##name}; \
    static void qilin_native_##name([[maybe_unused]] ::qilin::rt::Context& c)
// The same, checking the function's first instructions (encodings, not game
// data: e.g. 0x7C7603A6 is `mtspr DEC, r3`).
#define QILIN_NATIVE_CODE(name, ...)                                              \
    static void qilin_native_##name(::qilin::rt::Context& c);                     \
    static const ::qilin::rt::NativeRegistration qilin_reg_##name{#name, qilin_native_##name, {__VA_ARGS__}}; \
    static void qilin_native_##name([[maybe_unused]] ::qilin::rt::Context& c)

// Guest memory for native code: the same big-endian accessors lifted code
// uses, without the bring-up checks (native code may touch low memory and
// other places lifted code is stopped at).
inline uint32_t peek32(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, g_mem + a, 4);
    return std::byteswap(v);
}
inline void poke32(uint32_t a, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(g_mem + a, &v, 4);
}
inline uint16_t peek16(uint32_t a) {
    uint16_t v;
    std::memcpy(&v, g_mem + a, 2);
    return std::byteswap(v);
}
inline void poke16(uint32_t a, uint16_t v) {
    v = std::byteswap(v);
    std::memcpy(g_mem + a, &v, 2);
}
inline uint8_t peek8(uint32_t a) { return g_mem[a]; }
inline void poke8(uint32_t a, uint8_t v) { g_mem[a] = v; }

// A guest range native code reads or writes in bulk (a buffer and its
// length from the game): it must lie in MEM1 (low memory included) or
// MEM2, else the run stops naming it, instead of faulting on the unmapped
// gap or reaching past guest memory.
inline bool guest_range_ok(uint32_t addr, uint32_t length) {
    const uint64_t end = uint64_t(addr) + length;
    return (addr >= 0x80000000u && end <= 0x81800000u) || (addr >= 0x90000000u && end <= 0x94000000u);
}
inline void require_guest_range(uint32_t addr, uint32_t length, uint32_t pc, const char* what) {
    if (guest_range_ok(addr, length)) return;
    std::fprintf(stderr, "qilin_run: %s: %u bytes at %08X are outside guest memory\n", what, length, addr);
    throw Stop{Stop::Memory, pc, addr};
}
// A NUL-terminated guest string of at most `limit` characters; a longer one
// (or one running out of guest memory) stops instead of being cut short.
inline std::string guest_cstring(uint32_t addr, uint32_t limit, uint32_t pc, const char* what) {
    std::string s;
    for (uint32_t i = 0; i <= limit; ++i) {
        if (!guest_range_ok(addr + i, 1)) break;
        const char ch = static_cast<char>(peek8(addr + i));
        if (!ch) return s;
        s += ch;
    }
    std::fprintf(stderr, "qilin_run: %s: no string of at most %u characters at %08X\n", what, limit, addr);
    throw Stop{Stop::Unsupported, pc, addr};
}

}  // namespace qilin::rt
