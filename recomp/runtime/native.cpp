// The native-function registry (native.hpp). Runtime code: no std::format.
#include "native.hpp"

#include "interrupts.hpp"
#include "profile.hpp"

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
    uint32_t code[4];
    size_t code_words;
};
// Registrations run during static initialisation, in no particular order:
// the list lives in a function so it exists before the first of them.
std::vector<Pending>& pending() {
    static std::vector<Pending> list;
    return list;
}
std::vector<Fn> g_slots;  // by index in g_natives; nullptr: not registered
size_t g_bound = 0;

// The first of a registration's stated instructions the function at `addr`
// doesn't have; code_words if it has them all.
size_t first_mismatch(const Pending& p, uint32_t addr) {
    for (size_t k = 0; k < p.code_words; ++k)
        if (peek32(addr + 4 * static_cast<uint32_t>(k)) != p.code[k]) return k;
    return p.code_words;
}

}  // namespace

NativeRegistration::NativeRegistration(const char* name, Fn fn, std::initializer_list<uint32_t> code) {
    Pending p{name, fn, {}, 0};
    for (uint32_t w : code)
        if (p.code_words < 4) p.code[p.code_words++] = w;
    pending().push_back(p);
}

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
        } else if (p.code_words && !g_mem) {
            std::fprintf(stderr, "qilin_run: native %s states its code, but guest memory isn't loaded yet\n", p.name);
            ok = false;
        } else if (const size_t bad = first_mismatch(p, g_natives[found].addr); bad < p.code_words) {
            const uint32_t at = g_natives[found].addr + 4 * static_cast<uint32_t>(bad);
            std::fprintf(stderr,
                         "qilin_run: native %s: the function of that name (%08" PRIX32 ") isn't the expected code (word "
                         "%zu is %08" PRIX32 ", expected %08" PRIX32 "): the symbol list has put the name on the wrong "
                         "function\n",
                         p.name, g_natives[found].addr, bad, peek32(at), p.code[bad]);
            ok = false;
        } else {
            g_slots[found] = p.fn;
            ++g_bound;
        }
    }
    return ok;
}

size_t natives_bound() { return g_bound; }

NativeGuard g_native_guard = nullptr;

void native(Context& c, uint32_t index) {
    const Fn fn = g_slots[index];
    if (!fn) throw Stop{Stop::Unregistered, g_natives[index].addr, c.lr};
    // A safe point: the lifted caller has just stored the registers it
    // changed (r1 among them), so an interrupt can be taken here, before the
    // call, as the console takes one between two instructions. It keeps
    // the audio clock (an interrupt every 3 ms) from missing frames while a
    // thread is busy.
    deliver_due(c);
    if (g_native_guard) g_native_guard(c, g_natives[index]);
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

namespace {
thread_local const Context* t_context = nullptr;

// Stacks live in MEM1 or MEM2 (threads the game creates often use MEM2).
bool in_mem1(uint32_t a) {
    return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u);
}

void report_context() {
    if (!t_context) return;
    const Context& c = *t_context;
    std::fprintf(stderr, "qilin_run: registers (as last synced): r1 %08" PRIX32 " r3 %08" PRIX32 " r4 %08" PRIX32
                         " r5 %08" PRIX32 " r6 %08" PRIX32 " r7 %08" PRIX32 " r8 %08" PRIX32 " lr %08" PRIX32 "\n",
                 c.r[1], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.lr);
    report_stack(c);
}
}  // namespace

void report_stack(const Context& c) {
    // The back chain: each frame's first word is the caller's stack
    // pointer, and the caller's frame keeps the return address at +4.
    std::fprintf(stderr, "qilin_run: guest call stack:\n");
    uint32_t sp = c.r[1];
    for (unsigned depth = 0; depth < 24 && in_mem1(sp) && (sp & 7) == 0; ++depth) {
        const uint32_t back = peek32(sp);
        if (!in_mem1(back) || back <= sp) break;
        const uint32_t lr = peek32(back + 4);
        if (!in_mem1(lr)) break;
        char where[400];
        describe(where, sizeof where, lr);
        std::fprintf(stderr, "    %s\n", where);
        sp = back;
    }
}

void set_thread_context(const Context* c) { t_context = c; }

void report_stop(const Stop& s) {
    report_stats();
    report_context();
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

Stats& stats() {
    static Stats s;
    return s;
}

void report_stats() {
    const Stats& s = stats();
    std::fprintf(stderr,
                 "qilin_run: so far: %" PRIu64 " thread switches (%" PRIu64 " threads started), %" PRIu64
                 " interrupts (%" PRIu64 " from the decrementer), %" PRIu64 " files opened, %" PRIu64 " bytes read, %" PRIu64 " disc requests pending\n",
                 s.switches, s.threads_started, s.interrupts, s.decrementer, s.files_opened, s.bytes_read, s.dvd_pending);
    std::fprintf(stderr,
                 "qilin_run:          %" PRIu64 " draws (%" PRIu64 " vertices), %" PRIu64 " frames copied out, %" PRIu64
                 " copies to textures, %" PRIu64 " retraces, %" PRIu64 " audio frames (%" PRIu64 " mixed by the DSP, %" PRIu64
                 " voice frames), %" PRIu64 " remotes connected (%" PRIu64 " reports)\n",
                 s.draws, s.vertices, s.display_copies, s.texture_copies, s.retraces, s.audio_frames, s.dsp_frames,
                 s.voice_frames, s.remotes_connected, s.remote_reports);
    std::fprintf(stderr,
                 "qilin_run:          host: %" PRIu64 " frames presented, %" PRIu64 " texture uploads, %" PRIu64
                 " audio frames dropped (output behind), the draws in %" PRIu64 " batches\n",
                 s.frames_presented, s.textures_uploaded, s.audio_frames_dropped, s.draw_batches);
    if (s.audio_queue_count)
        std::fprintf(stderr,
                     "qilin_run:          audio out: queued %.1f ms on average (%.1f-%.1f), %" PRIu64
                     " underruns, resampling ratio %.5f; the game's clock at %.5fx\n",
                     double(s.audio_queue_sum) / double(s.audio_queue_count) / 128.0, double(s.audio_queue_min) / 128.0,
                     double(s.audio_queue_max) / 128.0, s.audio_underruns, s.audio_ratio, time_base_speed());
    if (g_report_extra)
        if (const std::string extra = g_report_extra(); !extra.empty()) std::fprintf(stderr, "qilin_run:          %s\n", extra.c_str());
    report_profile();
    std::fprintf(stderr,
                 "qilin_run:          %" PRIu64 " periodic interrupts dropped (late by a period); deliveries: %" PRIu64
                 " when idle, %" PRIu64 " in running code\n",
                 s.periods_dropped, s.idle_deliveries, s.running_deliveries);
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
