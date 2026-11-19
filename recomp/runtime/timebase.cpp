// The time base (guest.hpp): mftb reads it, and the runtime's timers run on
// it. Its own file, so the differential test, the runtime and the unit tests
// can all link it. Runtime code: no std::format.
#include <chrono>

#include "guest.hpp"

namespace qilin::rt {

// It runs at a quarter of the bus clock: 243 MHz / 4 on the Wii.
uint64_t time_base() {
#ifdef QILIN_DETERMINISTIC_TIME
    return kTimeStart + g_time_reads++ * kTimeStep;
#else
    using namespace std::chrono;
    const auto ns = static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    return ns / kNsPerSecond * kTimeBaseHz + ns % kNsPerSecond * kTimeBaseHz / kNsPerSecond;  // no overflow for centuries
#endif
}

}  // namespace qilin::rt
