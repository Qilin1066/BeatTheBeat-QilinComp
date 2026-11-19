// Interrupts at safe points (interrupts.hpp). Runtime code: no std::format.
#include "interrupts.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "native.hpp"

namespace qilin::rt {

namespace {

constexpr unsigned kDecrementer = 8;  // the decrementer's exception number
constexpr uint32_t kCurrentContext = 0x800000D4u;

// CPU state: touched only by the running guest thread.
bool g_enabled = false;
bool g_dec_armed = false;
uint64_t g_dec_deadline = 0;
std::array<uint32_t, 32> g_handlers{};
// Per host thread: a thread switch during a delivery's reschedule leaves
// this thread mid-delivery, not the one that runs next.
thread_local bool t_delivering = false;

struct Periodic {
    uint64_t due, period;
    std::function<void(Context&)> handler;
};
std::vector<Periodic> g_periodic;  // the running guest thread only

bool periodic_due(uint64_t now) {
    for (const Periodic& p : g_periodic)
        if (now >= p.due) return true;
    return false;
}

// The queue: any host thread may post.
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void(Context&)>> g_queue;
std::atomic<bool> g_queued{false};

uint32_t function(const char* name, uint32_t& cache) {
    if (!cache) cache = require_function(name);
    return cache;
}

// One queued handler, as __OSDispatchInterrupt runs one.
void run_posted(Context& c, const std::function<void(Context&)>& handler) {
    static uint32_t disable = 0, enable = 0, reschedule = 0;
    call_guest(c, function("OSDisableScheduler", disable));
    handler(c);
    call_guest(c, function("OSEnableScheduler", enable));
    call_guest(c, function("__OSReschedule", reschedule));
}

// The decrementer: the handler the OS installed, with the interrupted
// context. The SDK's ends in OSLoadContext, which unwinds back here.
void run_decrementer(Context& c) {
    const uint32_t handler = g_handlers[kDecrementer];
    if (!handler) throw Stop{Stop::Unsupported, 0, kDecrementer};  // a decrementer interrupt nothing handles
    c.r[3] = kDecrementer;
    c.r[4] = peek32(kCurrentContext);
    try {
        call(c, handler, 0);
    } catch (const Resume&) {
    }
}

void deliver(Context& c, bool decrementer) {
    const Context saved = c;
    const bool level = g_enabled;
    g_enabled = false;
    t_delivering = true;
    try {
        if (decrementer) {
            g_dec_armed = false;
            ++stats().interrupts, ++stats().decrementer;
            run_decrementer(c);
        }
        const uint64_t now = time_base();
        for (size_t i = 0; i < g_periodic.size(); ++i) {
            if (now < g_periodic[i].due) continue;
            g_periodic[i].due += g_periodic[i].period;
            if (g_periodic[i].due <= now) g_periodic[i].due = now + g_periodic[i].period;
            ++stats().interrupts;
            const auto handler = g_periodic[i].handler;  // a copy: the handler may add a timer
            run_posted(c, handler);
        }
        for (;;) {
            std::function<void(Context&)> handler;
            {
                std::lock_guard l(g_mutex);
                if (g_queue.empty()) {
                    g_queued.store(false, std::memory_order_relaxed);
                    break;
                }
                handler = std::move(g_queue.front());
                g_queue.pop_front();
            }
            ++stats().interrupts;
            run_posted(c, handler);
        }
    } catch (...) {
        t_delivering = false;
        throw;
    }
    t_delivering = false;
    g_enabled = level;
    c = saved;
}

bool decrementer_due() { return g_dec_armed && time_base() >= g_dec_deadline; }

}  // namespace

bool interrupts_enabled() { return g_enabled; }
void set_interrupts_enabled(bool on) { g_enabled = on; }
bool delivering() { return t_delivering; }

void post_interrupt(std::function<void(Context&)> handler) {
    {
        std::lock_guard l(g_mutex);
        g_queue.push_back(std::move(handler));
        g_queued.store(true, std::memory_order_release);
    }
    g_wake.notify_all();
}

void set_decrementer(uint32_t ticks) {
    g_dec_armed = true;
    g_dec_deadline = time_base() + ticks + 1;
}

uint32_t exception_handler(unsigned exception) { return g_handlers[exception & 31]; }
uint32_t set_exception_handler(unsigned exception, uint32_t handler) {
    const uint32_t old = g_handlers[exception & 31];
    g_handlers[exception & 31] = handler;
    return old;
}

void add_periodic(uint64_t period, std::function<void(Context&)> handler) {
    g_periodic.push_back(Periodic{time_base() + period, period, std::move(handler)});
}

void deliver_due(Context& c) {
    if (t_delivering || !g_enabled) return;
    const bool dec = decrementer_due();
    if (dec || g_queued.load(std::memory_order_acquire) || (!g_periodic.empty() && periodic_due(time_base())))
        deliver(c, dec);
}

bool wait_and_deliver(Context& c) {
    {
        std::unique_lock l(g_mutex);
        for (;;) {
            if (!g_queue.empty()) break;
            // The next timer: the decrementer or a periodic interrupt.
            bool any = g_dec_armed;
            uint64_t next = g_dec_deadline;
            for (const Periodic& p : g_periodic)
                if (!any || p.due < next) next = p.due, any = true;
            if (!any) return false;  // nothing queued, no timer, and nothing can queue one yet
            const uint64_t now = time_base();
            if (now >= next) break;
            const uint64_t ns = (next - now) * kNsPerSecond / kTimeBaseHz;
            g_wake.wait_for(l, std::chrono::nanoseconds(ns));
        }
    }
    deliver(c, decrementer_due());
    return true;
}

}  // namespace qilin::rt
