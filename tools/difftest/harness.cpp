// Differential-test harness: runs lifted functions on test cases from
// difftest.py and reports what they did, for comparison with the original
// code run under Unicorn.
//
//   qilin_difftest <main.dol>      cases on stdin, results on stdout
//
// Guest memory is MEM1 (24 MB at 0x80000000) holding the DOL image, as in
// the Unicorn run; anything else stops with a memory fault (checked memory).
// All integers are little-endian u32.
//   case:   fn, r0..r31, lr, ctr, cr, xer, f0..f31 (u64 bits), n, then n x (addr, len, bytes)
//           written over the image
//   result: status (0 = returned, else 1 + rt::Stop::Kind), stop pc, stop value,
//           r0..r31, lr, ctr, cr, xer, ret, f0..f31 (u64 bits), how many values non-IEEE
//           mode flushed to zero, then the changed 32-byte blocks of MEM1:
//           count, count x (addr, 32 bytes)
#include <cstdio>
#include <cstring>
#include <memory>
#include <print>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "dol.hpp"
#include "guest.hpp"

using namespace qilin;

namespace {

constexpr uint32_t kMem1 = kMem1Base;

// The whole 4 GB guest space, of which only MEM1 is ever touched (checked
// memory stops anything else).
uint8_t* reserve_guest_space() {
#if defined(_WIN32)
    // Windows has no overcommit: reserve the space and commit only MEM1, or
    // every parallel worker would charge 4 GB to the commit limit.
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    if (base && !VirtualAlloc(base + kMem1Base, kMem1Size, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
    return base;
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

bool read_u32(uint32_t& v) { return std::fread(&v, 4, 1, stdin) == 1; }
void write_u32(uint32_t v) { std::fwrite(&v, 4, 1, stdout); }
bool read_f64(double& d) {
    uint64_t v;
    if (std::fread(&v, 8, 1, stdin) != 1) return false;
    d = rt::dbl(v);
    return true;
}
void write_f64(double d) {
    const uint64_t v = rt::bits(d);
    std::fwrite(&v, 8, 1, stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: qilin_difftest <main.dol>");
        return 2;
    }
#if defined(_WIN32)
    // The protocol is binary: no CRLF translation, no 0x1A end of file.
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::println(stderr, "qilin_difftest: cannot set binary mode on stdin/stdout");
        return 2;
    }
#endif
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

    // Between cases, MEM1 and `initial` both hold the image (`baseline`). A
    // case's writes go into both; afterwards only the pages the function
    // changed and the written ranges are put back, so no case copies all of
    // MEM1.
    std::vector<uint8_t> baseline(kMem1Size, 0);
    std::memcpy(baseline.data() + (dol->base() - kMem1), dol->image().data(), dol->image().size());
    std::vector<uint8_t> initial = baseline;
    uint8_t* mem1 = rt::g_mem + kMem1;
    std::memcpy(mem1, baseline.data(), kMem1Size);
    constexpr uint32_t kPage = 4096;
    struct Range {
        uint32_t off, len;
    };
    std::vector<Range> written;
    std::vector<uint32_t> changed;

    for (uint32_t fn; read_u32(fn);) {
        rt::Context c;
        for (uint32_t& r : c.r) read_u32(r);
        read_u32(c.lr), read_u32(c.ctr), read_u32(c.cr), read_u32(c.xer);
        for (double& f : c.f) read_f64(f);
        uint32_t writes = 0;
        read_u32(writes);
        written.clear();
        for (uint32_t i = 0; i < writes; ++i) {
            uint32_t addr = 0, len = 0;
            read_u32(addr), read_u32(len);
            if (addr < kMem1 || addr - kMem1 + uint64_t(len) > kMem1Size ||
                std::fread(initial.data() + (addr - kMem1), 1, len, stdin) != len) {
                std::println(stderr, "qilin_difftest: bad write {:08X}+{}", addr, len);
                return 2;
            }
            std::memcpy(mem1 + (addr - kMem1), initial.data() + (addr - kMem1), len);
            written.push_back({addr - kMem1, len});
        }

        uint32_t status = 0, pc = 0, value = 0;
        rt::g_ni_flushes = 0;
        const uint32_t caller = c.lr;
        try {
            rt::call(c, fn, 0);
            // The check every lifted caller makes after a call: a function
            // that returns somewhere else (a smashed stack) stops.
            if (c.ret != caller) rt::bad_return(fn, c.ret);
        } catch (const rt::Stop& s) {
            status = 1 + s.kind;
            pc = s.pc;
            value = s.value;
        }
        write_u32(status), write_u32(pc), write_u32(value);
        for (uint32_t r : c.r) write_u32(r);
        write_u32(c.lr), write_u32(c.ctr), write_u32(c.cr), write_u32(c.xer), write_u32(c.ret);
        for (double f : c.f) write_f64(f);
        write_u32(rt::g_ni_flushes);
        changed.clear();
        for (uint32_t page = 0; page < kMem1Size; page += kPage) {
            if (std::memcmp(mem1 + page, initial.data() + page, kPage) == 0) continue;
            for (uint32_t off = page; off < page + kPage; off += 32)
                if (std::memcmp(mem1 + off, initial.data() + off, 32) != 0) changed.push_back(off);
        }
        write_u32(static_cast<uint32_t>(changed.size()));
        for (uint32_t off : changed) {
            write_u32(kMem1 + off);
            std::fwrite(mem1 + off, 1, 32, stdout);
        }
        std::fflush(stdout);

        // Back to the image: what the function changed, then the case's writes.
        for (uint32_t off : changed) std::memcpy(mem1 + off, baseline.data() + off, 32);
        for (const Range& w : written) {
            std::memcpy(mem1 + w.off, baseline.data() + w.off, w.len);
            std::memcpy(initial.data() + w.off, baseline.data() + w.off, w.len);
        }
    }
    return 0;
}
