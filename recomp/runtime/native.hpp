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
    NativeRegistration(const char* name, Fn fn);
};

// Joins registrations to g_natives. False, with the reason on stderr, if a
// registered name isn't a replaced function of this game, is ambiguous, or
// is registered twice: a replacement that can never run is a mistake.
bool bind_natives();
// How many of the game's replaced functions have native code.
size_t natives_bound();

#define QILIN_NATIVE(name)                                                        \
    static void qilin_native_##name(::qilin::rt::Context& c);                     \
    static const ::qilin::rt::NativeRegistration qilin_reg_##name{#name, qilin_native_##name}; \
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

}  // namespace qilin::rt
