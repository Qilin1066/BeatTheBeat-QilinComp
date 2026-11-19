// The profile's report (profile.hpp). Runtime code: no std::format.
#include "profile.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace qilin::rt {

namespace {

const auto g_start = std::chrono::steady_clock::now();

// The process's CPU time (user and system), in seconds.
double cpu_seconds() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto s = [](const FILETIME& f) { return double(uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
    return s(kernel) + s(user);
#else
    rusage u{};
    if (getrusage(RUSAGE_SELF, &u) != 0) return 0;
    auto s = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
    return s(u.ru_utime) + s(u.ru_stime);
#endif
}

}  // namespace

bool profiling() {
    static const bool on = [] {
        const char* v = std::getenv("QILIN_PROFILE");
        return v && *v && *v != '0';
    }();
    return on;
}

void report_profile() {
    if (!profiling()) return;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const double cpu = cpu_seconds();
    std::fprintf(stderr, "qilin_run: profile: %.1f s of CPU in %.1f s (%.0f%% of one core)\n", cpu, wall,
                 wall > 0 ? 100.0 * cpu / wall : 0.0);
    static const char* const names[] = {"draws (all of each, textures included)", "  of which textures (check, decode)",
                                        "frames (copy, submit, present)", "the mixer"};
    double sections = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(Section::Count); ++i) {
        const double s = double(g_section_ns[i].load()) / 1e9;
        const uint64_t n = g_section_calls[i].load();
        if (static_cast<Section>(i) != Section::Texture) sections += s;  // inside draws
        std::fprintf(stderr, "qilin_run:          %-38s %7.2f s  %9llu calls  %7.1f us each\n", names[i], s,
                     static_cast<unsigned long long>(n), n ? s * 1e6 / double(n) : 0.0);
    }
    std::fprintf(stderr, "qilin_run:          %-38s %7.2f s\n", "the rest (lifted code, OS, waits' CPU)", cpu - sections);
    std::fprintf(stderr,
                 "qilin_run:          a frame's peak use: %llu KB of vertices, %llu KB of uniforms, %llu KB of texture "
                 "uploads, %llu descriptor sets\n",
                 static_cast<unsigned long long>(g_peak_vertex_bytes >> 10), static_cast<unsigned long long>(g_peak_uniform_bytes >> 10),
                 static_cast<unsigned long long>(g_peak_upload_bytes >> 10), static_cast<unsigned long long>(g_peak_sets));
}

}  // namespace qilin::rt
