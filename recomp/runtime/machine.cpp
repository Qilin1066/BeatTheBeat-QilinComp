#include "machine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#else
#include <sys/mman.h>
#endif

namespace qilin::rt {

void fine_timers() {
#if defined(_WIN32)
    // Windows rounds short waits up to its 15.6 ms tick by default; the
    // audio clock needs 3 ms (the setting lasts until the process exits).
    timeBeginPeriod(1);
#endif
}

bool map_guest_memory() {
    constexpr uint64_t kSpace = 1ull << 32;
#if defined(_WIN32)
    // Windows has no overcommit: reserve the space, commit only the RAM.
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return false;
    if (!VirtualAlloc(base + kMem1, kMem1Bytes, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(base + kMem2, kMem2Bytes, MEM_COMMIT, PAGE_READWRITE))
        return false;
#else
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    auto* base = static_cast<uint8_t*>(p);
    if (mprotect(base + kMem1, kMem1Bytes, PROT_READ | PROT_WRITE) != 0 ||
        mprotect(base + kMem2, kMem2Bytes, PROT_READ | PROT_WRITE) != 0)
        return false;
#endif
    g_mem = base;
    return true;
}

namespace {
uint32_t g_scratch_next = 0, g_scratch_end = 0;
}

void set_scratch(uint32_t lo, uint32_t hi) { g_scratch_next = lo, g_scratch_end = hi; }

uint32_t guest_alloc(uint32_t bytes, uint32_t align) {
    const uint32_t at = (g_scratch_next + align - 1) & ~(align - 1);
    if (!g_scratch_end || at + bytes > g_scratch_end) {
        std::fprintf(stderr, "qilin_run: the runtime's guest scratch (%u bytes) is used up\n", kScratchBytes);
        throw Stop{Stop::Unsupported, 0, bytes};
    }
    g_scratch_next = at + bytes;
    std::memset(g_mem + at, 0, bytes);
    return at;
}

}  // namespace qilin::rt
