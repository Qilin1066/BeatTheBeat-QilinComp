// Pausing the game (pause.hpp). Runtime code: no std::format.
#include "pause.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "guest.hpp"

namespace qilin::rt {

namespace {

std::mutex g_mutex;
std::condition_variable g_changed;
bool g_requested = false, g_parked = false;
std::atomic<bool> g_requested_fast{false};  // read once a frame without the lock

}  // namespace

void request_pause(bool on) {
    {
        std::lock_guard l(g_mutex);
        g_requested = on;
        g_requested_fast.store(on, std::memory_order_release);
    }
    g_changed.notify_all();
}

bool pause_requested() { return g_requested_fast.load(std::memory_order_acquire); }

bool game_parked() {
    std::lock_guard l(g_mutex);
    return g_parked;
}

void park_if_requested() {
    if (!g_requested_fast.load(std::memory_order_acquire)) return;
    std::unique_lock l(g_mutex);
    if (!g_requested) return;
    const uint64_t stopped = time_base_unpaused();
    g_parked = true;
    g_changed.notify_all();
    g_changed.wait(l, [] { return !g_requested; });
    g_parked = false;
    skip_time_base(time_base_unpaused() - stopped);  // the game didn't see the pause go by
}

}  // namespace qilin::rt
