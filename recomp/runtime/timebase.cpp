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
}  // namespace

// The host clock in time-base ticks, pauses included.
uint64_t time_base_unpaused() {
    using namespace std::chrono;
    const auto ns = static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    return ns / kNsPerSecond * kTimeBaseHz + ns % kNsPerSecond * kTimeBaseHz / kNsPerSecond;  // no overflow for centuries
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
