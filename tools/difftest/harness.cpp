// Differential-test harness: runs lifted functions on test cases from
// difftest.py and reports what they did, for comparison with the original
// code run under Unicorn.
//
//   qilin_difftest <main.dol>      cases on stdin, results on stdout
//
// Guest memory is MEM1 (24 MB at 0x80000000) holding the DOL image, as in
// the Unicorn run; anything else stops with a memory fault (checked memory).
// All integers are little-endian u32.
//   case:   fn, r0..r31, lr, ctr, cr, xer, n, then n x (addr, len, bytes) written over the image
//   result: status (0 = returned, else 1 + rt::Stop::Kind), stop pc, stop value,
//           r0..r31, lr, ctr, cr, xer, ret, then the changed 32-byte blocks of MEM1:
//           count, count x (addr, 32 bytes)
#include <cstdio>
#include <cstring>
#include <memory>
#include <print>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "dol.hpp"
#include "guest.hpp"

using namespace qilin;

namespace {

constexpr uint32_t kMem1 = kMem1Base;

uint8_t* reserve_guest_space() {
#if defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

bool read_u32(uint32_t& v) { return std::fread(&v, 4, 1, stdin) == 1; }
void write_u32(uint32_t v) { std::fwrite(&v, 4, 1, stdout); }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: qilin_difftest <main.dol>");
        return 2;
    }
    auto dol = Dol::load(argv[1]);
    if (!dol) {
        std::println(stderr, "qilin_difftest: {}", dol.error());
        return 2;
    }
    rt::g_mem = reserve_guest_space();
    if (!rt::g_mem) {
        std::println(stderr, "qilin_difftest: cannot reserve 4 GB of address space");
        return 2;
    }
    rt::g_regions[0] = {kMem1, kMem1 + kMem1Size};
    rt::g_region_count = 1;

    std::vector<uint8_t> baseline(kMem1Size, 0), initial(kMem1Size);
    std::memcpy(baseline.data() + (dol->base() - kMem1), dol->image().data(), dol->image().size());
    uint8_t* mem1 = rt::g_mem + kMem1;

    for (uint32_t fn; read_u32(fn);) {
        rt::Context c;
        for (uint32_t& r : c.r) read_u32(r);
        read_u32(c.lr), read_u32(c.ctr), read_u32(c.cr), read_u32(c.xer);
        initial = baseline;
        uint32_t writes = 0;
        read_u32(writes);
        for (uint32_t i = 0; i < writes; ++i) {
            uint32_t addr = 0, len = 0;
            read_u32(addr), read_u32(len);
            if (addr < kMem1 || addr - kMem1 + uint64_t(len) > kMem1Size ||
                std::fread(initial.data() + (addr - kMem1), 1, len, stdin) != len) {
                std::println(stderr, "qilin_difftest: bad write {:08X}+{}", addr, len);
                return 2;
            }
        }
        std::memcpy(mem1, initial.data(), kMem1Size);

        uint32_t status = 0, pc = 0, value = 0;
        try {
            rt::call(c, fn, 0);
        } catch (const rt::Stop& s) {
            status = 1 + s.kind;
            pc = s.pc;
            value = s.value;
        }
        write_u32(status), write_u32(pc), write_u32(value);
        for (uint32_t r : c.r) write_u32(r);
        write_u32(c.lr), write_u32(c.ctr), write_u32(c.cr), write_u32(c.xer), write_u32(c.ret);
        std::vector<uint32_t> changed;
        for (uint32_t off = 0; off < kMem1Size; off += 32)
            if (std::memcmp(mem1 + off, initial.data() + off, 32) != 0) changed.push_back(off);
        write_u32(static_cast<uint32_t>(changed.size()));
        for (uint32_t off : changed) {
            write_u32(kMem1 + off);
            std::fwrite(mem1 + off, 1, 32, stdout);
        }
        std::fflush(stdout);
    }
    return 0;
}
