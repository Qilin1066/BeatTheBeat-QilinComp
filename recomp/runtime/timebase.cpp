// The time base (guest.hpp): mftb reads it, and the runtime's timers run on
// it. Its own file, so the differential test, the runtime and the unit tests
// can all link it. Runtime code: no std::format.
#include <atomic>
#include <chrono>

#include "guest.hpp"

namespace qilin::rt {

namespace {
// Time the game was paused for (the settings menu): it doesn't see it pass.
std::atomic<uint64_t> g_skipped{0};
// The game's speed against the host clock (set_time_base_speed), from the
// moment it was set: written once, before any thread but the main one runs.
double g_speed = 1.0;
uint64_t g_anchor_ns = 0, g_anchor_ticks = 0;

uint64_t host_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
uint64_t ticks_of(uint64_t ns) {
    return ns / kNsPerSecond * kTimeBaseHz + ns % kNsPerSecond * kTimeBaseHz / kNsPerSecond;  // no overflow for centuries
}
}  // namespace

void set_time_base_speed(double speed) {
    g_anchor_ns = host_ns();
    g_anchor_ticks = ticks_of(g_anchor_ns);  // continuous: the clock doesn't jump here
    g_speed = speed;
}
double time_base_speed() { return g_speed; }

uint64_t host_ns_for_ticks(uint64_t ticks) {
    const uint64_t ns = ticks / kTimeBaseHz * kNsPerSecond + ticks % kTimeBaseHz * kNsPerSecond / kTimeBaseHz;
    return g_speed == 1.0 ? ns : static_cast<uint64_t>(double(ns) / g_speed);
}

// The host clock in time-base ticks, pauses included.
uint64_t time_base_unpaused() {
    const uint64_t ns = host_ns();
    if (g_speed == 1.0) return ticks_of(ns);
    // From the anchor, so the double holds only the time since then (exact
    // to well under a tick for days) and the result is monotonic.
    return g_anchor_ticks + ticks_of(static_cast<uint64_t>(double(ns - g_anchor_ns) * g_speed));
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
