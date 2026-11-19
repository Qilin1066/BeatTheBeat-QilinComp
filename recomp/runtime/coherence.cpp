// Write generations per 4 KB page of MEM1 and MEM2 (coherence.hpp).
// Runtime code: no std::format.
#include "coherence.hpp"

#include <algorithm>
#include <vector>

#include "guest.hpp"

namespace qilin::rt {

namespace {

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kMem1Size = 24u << 20, kMem2Base = 0x10000000u, kMem2Size = 64u << 20;

uint64_t g_generation = 0;
std::vector<uint64_t> g_mem1(kMem1Size >> kPageShift), g_mem2(kMem2Size >> kPageShift);

// The pages [first, last] of a physical range, in the right array; false if
// the range isn't wholly in MEM1 or MEM2.
bool pages(uint32_t addr, uint32_t length, std::vector<uint64_t>*& v, uint32_t& first, uint32_t& last) {
    if (length == 0) return false;
    const uint32_t phys = addr & 0x1FFFFFFFu;
    const uint64_t end = uint64_t(phys) + length - 1;
    if (end < kMem1Size) {
        v = &g_mem1, first = phys >> kPageShift, last = static_cast<uint32_t>(end >> kPageShift);
        return true;
    }
    if (phys >= kMem2Base && end < uint64_t(kMem2Base) + kMem2Size) {
        v = &g_mem2, first = (phys - kMem2Base) >> kPageShift, last = static_cast<uint32_t>((end - kMem2Base) >> kPageShift);
        return true;
    }
    return false;
}

}  // namespace

void note_guest_write(uint32_t addr, uint32_t length) {
    std::vector<uint64_t>* v;
    uint32_t first, last;
    if (!pages(addr, length, v, first, last)) return;
    ++g_generation;
    std::fill(v->begin() + first, v->begin() + last + 1, g_generation);
}

uint64_t write_generation() { return g_generation; }

uint64_t newest_write(uint32_t addr, uint32_t length) {
    std::vector<uint64_t>* v;
    uint32_t first, last;
    if (!pages(addr, length, v, first, last)) return 0;
    return *std::max_element(v->begin() + first, v->begin() + last + 1);
}

// Lifted dcbf and dcbst (guest.hpp): a cache line (32 bytes) written back.
void cache_store(uint32_t ea) { note_guest_write(ea & ~31u, 32); }

}  // namespace qilin::rt
