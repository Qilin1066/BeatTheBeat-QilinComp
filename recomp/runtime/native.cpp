// The native-function registry (native.hpp). Runtime code: no std::format.
#include "native.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace qilin::rt {

namespace {

struct Pending {
    const char* name;
    Fn fn;
};
// Registrations run during static initialisation, in no particular order:
// the list lives in a function so it exists before the first of them.
std::vector<Pending>& pending() {
    static std::vector<Pending> list;
    return list;
}
std::vector<Fn> g_slots;  // by index in g_natives; nullptr: not registered
size_t g_bound = 0;

}  // namespace

NativeRegistration::NativeRegistration(const char* name, Fn fn) { pending().push_back({name, fn}); }

bool bind_natives() {
    g_slots.assign(g_native_count, nullptr);
    g_bound = 0;
    bool ok = true;
    for (const Pending& p : pending()) {
        size_t found = g_native_count, matches = 0;
        for (size_t k = 0; k < g_native_count; ++k)
            if (std::strcmp(g_natives[k].name, p.name) == 0) found = k, ++matches;
        if (matches != 1) {
            std::fprintf(stderr, "qilin_run: native %s: %s\n", p.name,
                         matches ? "several replaced functions have this name"
                                 : "the game has no replaced function of this name");
            ok = false;
        } else if (g_slots[found]) {
            std::fprintf(stderr, "qilin_run: native %s is registered twice\n", p.name);
            ok = false;
        } else {
            g_slots[found] = p.fn;
            ++g_bound;
        }
    }
    return ok;
}

size_t natives_bound() { return g_bound; }

void native(Context& c, uint32_t index) {
    const Fn fn = g_slots[index];
    if (!fn) throw Stop{Stop::Unregistered, g_natives[index].addr, c.lr};
    fn(c);
    c.ret = c.lr;  // as the function's blr
}

uint32_t require_function(const char* name) {
    const uint32_t a = symbol_address(name);
    if (!a) {
        std::fprintf(stderr, "qilin_run: the lift has no (or more than one) function named %s\n", name);
        std::fflush(stderr);
        std::_Exit(2);
    }
    return a;
}

void call_guest(Context& c, uint32_t fn) {
    const uint32_t lr = c.lr;
    call(c, fn, lr);
    if (c.ret != lr) bad_return(fn, c.ret);
    c.lr = lr;
}

namespace {
void describe(char* out, size_t n, uint32_t addr) {
    const Symbol* s = symbol_containing(addr);
    if (!s) std::snprintf(out, n, "%08" PRIX32, addr);
    else if (s->addr == addr) std::snprintf(out, n, "%08" PRIX32 " (%s)", addr, s->name);
    else std::snprintf(out, n, "%08" PRIX32 " (%s+0x%" PRIX32 ")", addr, s->name, addr - s->addr);
}
}  // namespace

void report_stop(const Stop& s) {
    char pc[400], value[400];
    describe(pc, sizeof pc, s.pc);
    describe(value, sizeof value, s.value);
    switch (s.kind) {
    case Stop::Unregistered: {
        const char* library = "?";
        for (size_t k = 0; k < g_native_count; ++k)
            if (g_natives[k].addr == s.pc) library = g_natives[k].library;
        std::fprintf(stderr, "qilin_run: stopped: no native code yet for %s [%s], called from %s\n", pc, library, value);
        break;
    }
    case Stop::Memory:
        std::fprintf(stderr, "qilin_run: stopped: access to %08" PRIX32 ", outside the memory lifted code may use, at %s\n",
                     s.value, pc);
        break;
    case Stop::BadJump:
        std::fprintf(stderr, "qilin_run: stopped: indirect call or jump to %s, which is not a function, at %s\n", value, pc);
        break;
    case Stop::BadReturn: std::fprintf(stderr, "qilin_run: stopped: %s returned to %s instead of its caller\n", pc, value); break;
    case Stop::Trap: std::fprintf(stderr, "qilin_run: stopped: trap at %s\n", pc); break;
    case Stop::NotLifted: std::fprintf(stderr, "qilin_run: stopped: %s is not lifted\n", pc); break;
    case Stop::Unsupported:
        std::fprintf(stderr, "qilin_run: stopped: unsupported at %s (value %08" PRIX32 ")\n", pc, s.value);
        break;
    case Stop::Idle:
        std::fprintf(stderr,
                     "qilin_run: stopped: no thread can run (the last, %08" PRIX32 ", went to sleep), and nothing the "
                     "runtime provides yet would wake one\n",
                     s.value);
        break;
    }
}

const Symbol* symbol_at(uint32_t addr) {
    const Symbol* s = symbol_containing(addr);
    return s && s->addr == addr ? s : nullptr;
}

const Symbol* symbol_containing(uint32_t addr) {
    const Symbol* end = g_symbols + g_symbol_count;
    const Symbol* it = std::upper_bound(g_symbols, end, addr, [](uint32_t a, const Symbol& s) { return a < s.addr; });
    return it == g_symbols ? nullptr : it - 1;
}

uint32_t symbol_address(const char* name) {
    uint32_t addr = 0;
    for (size_t i = 0; i < g_symbol_count; ++i) {
        if (std::strcmp(g_symbols[i].name, name) != 0) continue;
        if (addr) return 0;
        addr = g_symbols[i].addr;
    }
    return addr;
}

}  // namespace qilin::rt
