// Interrupts at safe points (interrupts.hpp). Runtime code: no std::format.
#include "interrupts.hpp"

#include <algorithm>
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
thread_local bool t_delivering = false;      // inside a delivery: no nested one
thread_local bool t_in_decrementer = false;  // inside the decrementer's handler: OSLoadContext unwinds

constexpr unsigned kMaxCatchUp = 32;  // periods one delivery runs at most (about 100 ms of audio)

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

// The queue: any host thread may post. Each handler has the time it may run
// from; g_next_due is the earliest (kNever when the queue is empty), so safe
// points check it without the lock.
constexpr uint64_t kNever = ~uint64_t(0);
struct Queued {
    uint64_t due;
    std::function<void(Context&)> handler;
};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Queued> g_queue;
std::atomic<uint64_t> g_next_due{kNever};

// Lifted loops test g_irq_pending on every backward branch: it says
// something is due now. What comes due later (an I/O completion 1 ms
// ahead) is found by the loops' every-1024th-time check and at every native
// call, so a loop doesn't take the slow path while a completion waits.
void update_next_due() {  // under g_mutex
    uint64_t next = kNever;
    for (const Queued& q : g_queue) next = q.due < next ? q.due : next;
    g_next_due.store(next, std::memory_order_release);
    g_irq_pending.store(next != kNever && next <= time_base(), std::memory_order_relaxed);
}

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
    // Only here may OSLoadContext unwind (delivering()): from any other
    // handler it stops, naming itself, rather than escape uncaught.
    t_in_decrementer = true;
    try {
        call(c, handler, 0);
    } catch (const Resume&) {
    } catch (...) {
        t_in_decrementer = false;
        throw;
    }
    t_in_decrementer = false;
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
        // Each period that has come runs, in turn: on the console every one
        // arrives on time (the audio DMA, the retrace), so a late wake-up
        // here catches up rather than losing them. Only a stall longer than
        // kMaxCatchUp drops the rest, and counts them.
        const uint64_t now = time_base();
        for (size_t i = 0; i < g_periodic.size(); ++i) {
            for (unsigned n = 0; now >= g_periodic[i].due; ++n) {
                if (n == kMaxCatchUp) {
                    stats().periods_dropped += (now - g_periodic[i].due) / g_periodic[i].period + 1;
                    g_periodic[i].due = now + g_periodic[i].period;
                    break;
                }
                g_periodic[i].due += g_periodic[i].period;
                ++stats().interrupts;
                const auto handler = g_periodic[i].handler;  // a copy: the handler may add a timer
                run_posted(c, handler);
            }
        }
        // The queued handlers that are due, in the order they were posted.
        for (;;) {
            std::function<void(Context&)> handler;
            {
                std::lock_guard l(g_mutex);
                const uint64_t t = time_base();
                const auto it = std::find_if(g_queue.begin(), g_queue.end(), [t](const Queued& q) { return q.due <= t; });
                if (it == g_queue.end()) {
                    update_next_due();
                    break;
                }
                handler = std::move(it->handler);
                g_queue.erase(it);
                update_next_due();
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
bool delivering() { return t_in_decrementer; }

void post_interrupt(std::function<void(Context&)> handler, uint64_t delay) {
    {
        std::lock_guard l(g_mutex);
        g_queue.push_back(Queued{time_base() + delay, std::move(handler)});
        update_next_due();  // lifted loops start looking
    }
    g_wake.notify_all();
}

void poll(Context& c) { deliver_due(c); }

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
    const uint64_t now = time_base();
    const bool dec = g_dec_armed && now >= g_dec_deadline;
    if (dec || now >= g_next_due.load(std::memory_order_acquire) || (!g_periodic.empty() && periodic_due(now))) {
        ++stats().running_deliveries;
        deliver(c, dec);
    }
}

bool wait_and_deliver(Context& c) {
    {
        std::unique_lock l(g_mutex);
        for (;;) {
            // The next event: a queued handler, the decrementer or a
            // periodic interrupt.
            bool any = !g_queue.empty();
            uint64_t next = g_next_due.load(std::memory_order_relaxed);
            if (g_dec_armed && (!any || g_dec_deadline < next)) next = g_dec_deadline, any = true;
            for (const Periodic& p : g_periodic)
                if (!any || p.due < next) next = p.due, any = true;
            if (!any) return false;  // nothing queued, no timer, and nothing can queue one yet
            const uint64_t now = time_base();
            if (now >= next) break;
            const uint64_t ns = (next - now) * kNsPerSecond / kTimeBaseHz;
            g_wake.wait_for(l, std::chrono::nanoseconds(ns));
        }
    }
    ++stats().idle_deliveries;
    deliver(c, decrementer_due());
    return true;
}

}  // namespace qilin::rt
