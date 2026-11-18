// The native-function registry (native.hpp). Runtime code: no std::format.
#include "native.hpp"

#include <algorithm>
#include <cstdio>
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
