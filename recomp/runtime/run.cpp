// qilin_run: the game.
//
//   qilin_run <main.dol>
//
// Loads the DOL into MEM1 and runs start-up as the SDK's __start does, with
// native code where the plan replaces a library (docs/code-analysis.md).
// Whatever the runtime doesn't provide yet stops the game with a report: the
// replaced function, the hardware address or the instruction, and where it
// was reached from. Exit status: 0 the game ended itself, 1 it stopped,
// 2 the runtime couldn't start. Runtime code: no std::format.
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "dol.hpp"
#include "machine.hpp"
#include "native.hpp"

using namespace qilin;

namespace {

// What __start has done by the time it calls OSInit, in the supported build
// (docs/dol-analysis.md): __init_registers' stack pointer and small-data
// bases, the 8-byte terminating stack frame, and __init_hardware's
// paired-single set-up (HID2's LSQE and PSE bits; the GQRs cleared).
// __init_data has nothing to do: its ROM copy table maps each section onto
// itself and the loader leaves BSS zeroed.
constexpr uint32_t kStackTop = 0x803E7488u, kSmallData2 = 0x8032A680u, kSmallData = 0x80328500u;
constexpr uint32_t kHid2 = 920, kHid2Lsqe = 0x80000000u, kHid2Pse = 0x20000000u;
// __start's return addresses, so the game sees the link register it would.
constexpr uint32_t kAfterOSInit = 0x80004170u, kAfterInitUser = 0x800041A8u, kAfterMain = 0x800041B4u;

void describe(char* out, size_t n, uint32_t addr) {
    const rt::Symbol* s = rt::symbol_containing(addr);
    if (!s) std::snprintf(out, n, "%08" PRIX32, addr);
    else if (s->addr == addr) std::snprintf(out, n, "%08" PRIX32 " (%s)", addr, s->name);
    else std::snprintf(out, n, "%08" PRIX32 " (%s+0x%" PRIX32 ")", addr, s->name, addr - s->addr);
}

void report(const rt::Stop& s) {
    char pc[400], value[400];
    describe(pc, sizeof pc, s.pc);
    describe(value, sizeof value, s.value);
    switch (s.kind) {
    case rt::Stop::Unregistered: {
        const char* library = "?";
        for (size_t k = 0; k < rt::g_native_count; ++k)
            if (rt::g_natives[k].addr == s.pc) library = rt::g_natives[k].library;
        std::fprintf(stderr, "qilin_run: stopped: no native code yet for %s [%s], called from %s\n", pc, library, value);
        break;
    }
    case rt::Stop::Memory:
        std::fprintf(stderr, "qilin_run: stopped: access to %08" PRIX32 ", outside the memory lifted code may use, at %s\n",
                     s.value, pc);
        break;
    case rt::Stop::BadJump:
        std::fprintf(stderr, "qilin_run: stopped: indirect call or jump to %s, which is not a function, at %s\n", value, pc);
        break;
    case rt::Stop::BadReturn:
        std::fprintf(stderr, "qilin_run: stopped: %s returned to %s instead of its caller\n", pc, value);
        break;
    case rt::Stop::Trap: std::fprintf(stderr, "qilin_run: stopped: trap at %s\n", pc); break;
    case rt::Stop::NotLifted: std::fprintf(stderr, "qilin_run: stopped: %s is not lifted\n", pc); break;
    case rt::Stop::Unsupported:
        std::fprintf(stderr, "qilin_run: stopped: unsupported at %s (value %08" PRIX32 ")\n", pc, s.value);
        break;
    }
}

uint32_t need(const char* name) {
    const uint32_t a = rt::symbol_address(name);
    if (!a) {
        std::fprintf(stderr, "qilin_run: the lifted code has no (or more than one) function named %s\n", name);
        throw rt::Exit{2};
    }
    return a;
}

// Calls a guest function as a `bl` at return_to - 4 would.
void call_from(rt::Context& c, uint32_t fn, uint32_t return_to) {
    c.lr = return_to;
    rt::call(c, fn, return_to - 4);
    if (c.ret != return_to) rt::bad_return(fn, c.ret);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: qilin_run <main.dol>\n");
        return 2;
    }
    const auto dol = Dol::load(argv[1]);
    if (!dol) {
        std::fprintf(stderr, "qilin_run: %s: %s\n", argv[1], dol.error().c_str());
        return 2;
    }
    if (!known_build(dol->sha256)) {
        std::fprintf(stderr, "qilin_run: %s is not the supported build (SHA-256 %s)\n", argv[1], dol->sha256.c_str());
        return 2;
    }
    if (!rt::bind_natives()) return 2;
    if (!rt::map_guest_memory()) {
        std::fprintf(stderr, "qilin_run: cannot map the guest's memory (4 GB of address space, 88 MB of RAM)\n");
        return 2;
    }
    std::memcpy(rt::g_mem + dol->base(), dol->image().data(), dol->image().size());
#ifdef QILIN_CHECKED_MEMORY
    // Bring-up: lifted code may use MEM1 from the DOL's base up, and MEM2.
    // Low memory (the loader's and the OS's globals below the DOL), the
    // uncached mirrors and the hardware registers stop, so each one lifted
    // code really uses is found and then provided on purpose.
    rt::g_regions[0] = {dol->base(), rt::kMem1 + rt::kMem1Bytes};
    rt::g_regions[1] = {rt::kMem2, rt::kMem2 + rt::kMem2Bytes};
    rt::g_region_count = 2;
#endif
    std::fprintf(stderr, "qilin_run: %zu of %zu replaced functions have native code\n", rt::natives_bound(),
                 rt::g_native_count);

    rt::Context c;
    c.r[1] = kStackTop - 8;
    rt::poke32(c.r[1], 0xFFFFFFFFu);
    rt::poke32(c.r[1] + 4, 0xFFFFFFFFu);
    c.r[2] = kSmallData2;
    c.r[13] = kSmallData;
    c.spr[kHid2] = kHid2Lsqe | kHid2Pse;
    try {
        call_from(c, need("OSInit"), kAfterOSInit);
        call_from(c, need("__init_user"), kAfterInitUser);
        c.r[3] = 0;  // argc, argv: none (the disc's bi2 passes no arguments)
        c.r[4] = 0;
        call_from(c, need("main"), kAfterMain);
        // __start branches to exit without linking: it returns nowhere.
        c.lr = kAfterMain;
        rt::call(c, need("exit"), kAfterMain);
        std::fprintf(stderr, "qilin_run: stopped: exit returned\n");
        return 1;
    } catch (const rt::Stop& s) {
        report(s);
        return 1;
    } catch (const rt::Exit& e) {
        return e.code;
    }
}
