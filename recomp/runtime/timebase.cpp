// The time base (guest.hpp): mftb reads it, and the runtime's timers run on
// it. Its own file, so the differential test, the runtime and the unit tests
// can all link it. Runtime code: no std::format.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

#include "guest.hpp"

namespace qilin::rt {

namespace {
// Time the game was paused for (the settings menu): it doesn't see it pass.
std::atomic<uint64_t> g_skipped{0};

uint64_t host_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
uint64_t ticks_of(uint64_t ns) {
    return ns / kNsPerSecond * kTimeBaseHz + ns % kNsPerSecond * kTimeBaseHz / kNsPerSecond;  // no overflow for centuries
}

// The game's speed against the host clock (set_time_base_speed), from the
// moment it was set: the display lock sets it at start, and the vblank lock
// steers it by a little while the game runs. Each setting is a new entry in
// a ring and the index moves to it, so a reader on any thread sees one whole
// entry; updates come a few times a second at most, so the ring never comes
// round to an entry still being read.
struct Rate {
    uint64_t anchor_ns = 0, anchor_ticks = 0;  // the clock at the moment it was set
    double speed = 1.0;
};
constexpr unsigned kRates = 256;
std::array<Rate, kRates> g_rates{};  // [0]: anchored at 0, speed 1 (the host clock)
std::atomic<unsigned> g_rate{0};
std::mutex g_rate_lock;               // writers
std::atomic<uint64_t> g_latest{0};   // the clock never goes back (a reader on the old rate)

uint64_t ticks_at(const Rate& r, uint64_t ns) {
    if (ns <= r.anchor_ns) return r.anchor_ticks;
    const uint64_t since = ns - r.anchor_ns;
    // At speed 1 in integers; otherwise the double holds only the time since
    // the anchor (exact to well under a tick for days).
    return r.anchor_ticks + ticks_of(r.speed == 1.0 ? since : static_cast<uint64_t>(double(since) * r.speed));
}
}  // namespace

void set_time_base_speed(double speed) {
    std::lock_guard l(g_rate_lock);
    const unsigned i = g_rate.load(std::memory_order_relaxed);
    if (g_rates[i % kRates].speed == speed) return;
    const uint64_t ns = host_ns();
    Rate next;
    next.anchor_ns = ns;
    next.anchor_ticks = std::max(ticks_at(g_rates[i % kRates], ns), g_latest.load(std::memory_order_relaxed));  // continuous
    next.speed = speed;
    g_rates[(i + 1) % kRates] = next;
    g_rate.store(i + 1, std::memory_order_release);
}
double time_base_speed() { return g_rates[g_rate.load(std::memory_order_acquire) % kRates].speed; }

uint64_t host_ns_for_ticks(uint64_t ticks) {
    const uint64_t ns = ticks / kTimeBaseHz * kNsPerSecond + ticks % kTimeBaseHz * kNsPerSecond / kTimeBaseHz;
    const double speed = time_base_speed();
    return speed == 1.0 ? ns : static_cast<uint64_t>(double(ns) / speed);
}

// The host clock in time-base ticks, pauses included.
uint64_t time_base_unpaused() {
    const uint64_t v = ticks_at(g_rates[g_rate.load(std::memory_order_acquire) % kRates], host_ns());
    uint64_t prev = g_latest.load(std::memory_order_relaxed);
    while (v > prev && !g_latest.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
    return v > prev ? v : prev;
}
void skip_time_base(uint64_t ticks) { g_skipped.fetch_add(ticks, std::memory_order_relaxed); }

// It runs at a quarter of the bus clock: 243 MHz / 4 on the Wii.
uint64_t time_base() {
#ifdef QILIN_DETERMINISTIC_TIME
    return kTimeStart + g_time_reads++ * kTimeStep;
#else
    return time_base_unpaused() - g_skipped.load(std::memory_order_relaxed);
#endif
}

}  // namespace qilin::rt
