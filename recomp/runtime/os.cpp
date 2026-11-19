// revolution/os and revolution/base, native: what lifted code calls of them
// (the runtime's to-do list, tools/analysis/runtime_todo.py), added as
// bring-up reaches each one. Runtime code: no std::format.
#include <cstdio>
#include <vector>

#include "interrupts.hpp"
#include "machine.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

// ---- What the loader hands OSInit ----
// OSInit takes the arenas from low memory, which IOS and the apploader fill
// in before the game starts; nothing in the DOL says what they put there.
//  - MEM1's arena starts where OSInit puts it when low memory gives no start
//    (0x8000310C = 0): the end of the debugger stack that follows the main
//    stack, rounded up to 32 bytes.
//  - MEM1's arena end is OSInit's own default for when neither low memory
//    (0x80003110) nor the boot information (0x80000034) gives one. A console
//    gives one (the start of the disc's file table, which the native DVD
//    code keeps outside guest memory), so this is on the small side.
//  - MEM2's arena: OSInit raises a start below 0x90000800 to that, and takes
//    the end as IOS gives it; 0x933E0000 is the documented start of IOS's own
//    heap at the top of MEM2.
// TO CONFIRM against the original game in Dolphin (the reference): dump
// 0x80000000-0x80003400 at the entry point and set these from it.
constexpr uint32_t kMem1ArenaLo = 0x803E94A0u, kMem1ArenaHi = 0x81730000u;
constexpr uint32_t kMem2ArenaLo = 0x90000800u, kMem2ArenaHi = 0x933E0000u;

// The clocks the SDK's tick conversions read from low memory: the bus at
// 243 MHz (the time base runs at a quarter of it) and the CPU at 729 MHz.
// TO CONFIRM with lowmem.py, as the arenas above.
constexpr uint32_t kBusClockAddr = 0x800000F8u, kCpuClockAddr = 0x800000FCu;
constexpr uint32_t kBusClock = 243'000'000u, kCpuClock = 729'000'000u;
static_assert(kBusClock / 4 == kTimeBaseHz);

struct Os {
    bool initialised = false;
    uint32_t mem1_lo = 0, mem1_hi = 0, mem2_lo = 0, mem2_hi = 0;
    std::vector<uint32_t> shutdown_functions;  // OSRegisterShutdownFunction, for when shutdown is native
} os;

// ---- start-up and shutdown ----
QILIN_NATIVE(OSInit) {
    if (os.initialised) return;  // as the original: a second call does nothing
    os.initialised = true;
    // The runtime's guest scratch comes off the top of the MEM1 arena, so the
    // game's heaps never reach it.
    os.mem1_lo = kMem1ArenaLo, os.mem1_hi = kMem1ArenaHi - kScratchBytes;
    set_scratch(os.mem1_hi, kMem1ArenaHi);
    os.mem2_lo = kMem2ArenaLo, os.mem2_hi = kMem2ArenaHi;
    poke32(kBusClockAddr, kBusClock);
    poke32(kCpuClockAddr, kCpuClock);
    // The initialisers of the OS units that are lifted, in OSInit's order.
    call_guest(c, require_function("__OSInitAlarm"));
    call_guest(c, require_function("__OSThreadInit"));
    set_interrupts_enabled(true);  // OSInit ends with OSEnableInterrupts
}

QILIN_NATIVE(OSRegisterShutdownFunction) { os.shutdown_functions.push_back(c.r[3]); }

// base: where exit ends after the static destructors (sync; nop; li r3,0).
QILIN_NATIVE_CODE(PPCHalt, 0x7C0004ACu, 0x60000000u, 0x38600000u) { throw Exit{0}; }

// ---- interrupts ----
// Each returns whether interrupts were enabled before. Enabling them is a
// safe point: what is due is delivered there.
QILIN_NATIVE(OSDisableInterrupts) {
    c.r[3] = interrupts_enabled();
    set_interrupts_enabled(false);
}
QILIN_NATIVE(OSEnableInterrupts) {
    const bool was = interrupts_enabled();
    set_interrupts_enabled(true);
    deliver_due(c);
    c.r[3] = was;
}
QILIN_NATIVE(OSRestoreInterrupts) {
    const bool level = c.r[3] != 0, was = interrupts_enabled();
    set_interrupts_enabled(level);
    if (level) deliver_due(c);
    c.r[3] = was;
}

// ---- exceptions and the decrementer ----
QILIN_NATIVE(__OSSetExceptionHandler) { c.r[3] = set_exception_handler(c.r[3] & 0xFF, c.r[4]); }
QILIN_NATIVE(__OSGetExceptionHandler) { c.r[3] = exception_handler(c.r[3] & 0xFF); }
// mtspr DEC, r3; blr. The alarm code sets the next deadline through it.
QILIN_NATIVE_CODE(PPCMtdec, 0x7C7603A6u, 0x4E800020u) { set_decrementer(c.r[3]); }

// ---- arenas ----
QILIN_NATIVE(OSGetMEM1ArenaLo) { c.r[3] = os.mem1_lo; }
QILIN_NATIVE(OSGetMEM1ArenaHi) { c.r[3] = os.mem1_hi; }
QILIN_NATIVE(OSGetMEM2ArenaLo) { c.r[3] = os.mem2_lo; }
QILIN_NATIVE(OSGetMEM2ArenaHi) { c.r[3] = os.mem2_hi; }
QILIN_NATIVE(OSGetArenaLo) { c.r[3] = os.mem1_lo; }  // the plain arena is MEM1's
QILIN_NATIVE(OSGetArenaHi) { c.r[3] = os.mem1_hi; }
QILIN_NATIVE(OSSetMEM1ArenaLo) { os.mem1_lo = c.r[3]; }
QILIN_NATIVE(OSSetMEM1ArenaHi) { os.mem1_hi = c.r[3]; }
QILIN_NATIVE(OSSetMEM2ArenaLo) { os.mem2_lo = c.r[3]; }
QILIN_NATIVE(OSSetMEM2ArenaHi) { os.mem2_hi = c.r[3]; }
QILIN_NATIVE(OSSetArenaLo) { os.mem1_lo = c.r[3]; }
QILIN_NATIVE(OSSetArenaHi) { os.mem1_hi = c.r[3]; }

// ---- caches ----
// Guest memory has no caches behind it, so storing, flushing and
// invalidating ranges have nothing to do (as dcbf and icbi in lifted code).
QILIN_NATIVE(DCStoreRange) {}
QILIN_NATIVE(DCFlushRange) {}
QILIN_NATIVE(DCInvalidateRange) {}
QILIN_NATIVE(ICInvalidateRange) {}

// ---- the locked cache ----
// A 16 KB scratchpad at 0xE0000000 that DMA fills and empties. NW4R turns it
// on and off around its work, but no lifted code addresses it (nothing
// outside the replaced libraries builds 0xE000xxxx, and only the debugger
// stub touches the DMA registers): so there is no DMA, and an access to it
// would stop under checked memory.
bool g_lc_enabled = false;
QILIN_NATIVE(LCEnable) { g_lc_enabled = true; }
QILIN_NATIVE(LCDisable) { g_lc_enabled = false; }
QILIN_NATIVE(LCQueueLength) { c.r[3] = 0; }  // no DMA transfers pending

// ---- reports ----
// OSRegisterVersion(const char* id) reports the library's version string.
QILIN_NATIVE(OSRegisterVersion) {
    std::fprintf(stderr, "[game] %s\n", reinterpret_cast<const char*>(g_mem + c.r[3]));
}

}  // namespace
