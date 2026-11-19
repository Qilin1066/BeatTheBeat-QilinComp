// QILIN_PROFILE=1: where the runtime's CPU time goes, for efficiency work
// (the low-end target is a Raspberry Pi 4). Sections are timed with the
// host's steady clock and summed; the report (profile.cpp) adds the
// process's CPU time against the wall clock, so what's left over is lifted
// code and the OS. Off, a section costs one well-predicted branch.
// Runtime code: no std::format.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace qilin::rt {

enum class Section : unsigned { Draw, Texture, Pipeline, Present, Mixer, Count };

bool profiling();
inline std::atomic<uint64_t> g_section_ns[static_cast<unsigned>(Section::Count)];
inline std::atomic<uint64_t> g_section_calls[static_cast<unsigned>(Section::Count)];

class Timed {
public:
    explicit Timed(Section s) : s_(s), on_(profiling()) {
        if (on_) start_ = std::chrono::steady_clock::now();
    }
    ~Timed() {
        if (!on_) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
        g_section_ns[static_cast<unsigned>(s_)].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
        g_section_calls[static_cast<unsigned>(s_)].fetch_add(1, std::memory_order_relaxed);
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

private:
    Section s_;
    bool on_;
    std::chrono::steady_clock::time_point start_;
};

// The renderer's per-frame ring use, for sizing them (the peaks so far).
inline uint64_t g_peak_vertex_bytes = 0, g_peak_uniform_bytes = 0, g_peak_upload_bytes = 0, g_peak_sets = 0;
inline void note_ring_peaks(uint64_t vtx, uint64_t ubo, uint64_t upload, uint64_t sets) {
    if (vtx > g_peak_vertex_bytes) g_peak_vertex_bytes = vtx;
    if (ubo > g_peak_uniform_bytes) g_peak_uniform_bytes = ubo;
    if (upload > g_peak_upload_bytes) g_peak_upload_bytes = upload;
    if (sets > g_peak_sets) g_peak_sets = sets;
}

// The GPU's time for each frame (timestamp queries; the renderer's frames).
inline uint64_t g_gpu_frames = 0, g_gpu_ns = 0, g_gpu_max_ns = 0;
inline void note_gpu_frame(uint64_t ns) {
    ++g_gpu_frames, g_gpu_ns += ns;
    if (ns > g_gpu_max_ns) g_gpu_max_ns = ns;
}

// The profile so far, on stderr (with the stop or time-limit report).
void report_profile();

}  // namespace qilin::rt
