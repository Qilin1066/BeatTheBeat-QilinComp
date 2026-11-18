#include "machine.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace qilin::rt {

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

}  // namespace qilin::rt
