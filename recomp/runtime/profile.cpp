// The profile's report (profile.hpp). Runtime code: no std::format.
#include "profile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#else
#include <sys/resource.h>
#endif

namespace qilin::rt {

namespace {

const auto g_start = std::chrono::steady_clock::now();

// QILIN_SAMPLE=1 (Windows): a sampling profiler. Every millisecond a thread
// pauses each of the process's other threads in turn, reads where it is and
// lets it go on; the report names the hottest functions from the debug
// symbols (a build with QILIN_DEBUG_SYMBOLS). Nothing is allocated while a
// thread is paused (it may hold the heap's lock). Threads found waiting in
// the OS count as idle, apart.
#if defined(_WIN32)
std::mutex g_sample_lock;
std::unordered_map<uint64_t, uint64_t> g_samples;  // address -> samples
uint64_t g_sample_ticks = 0;

void sample_forever() {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::unordered_map<DWORD, HANDLE> threads;
    std::vector<uint64_t> found;
    found.reserve(256);
    for (unsigned tick = 0;; ++tick) {
        if (tick % 100 == 0) {  // new threads (each guest thread has its own)
            const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && !threads.contains(te.th32ThreadID))
                    if (const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                                    FALSE, te.th32ThreadID))
                        threads.emplace(te.th32ThreadID, h);
            CloseHandle(snap);
        }
        found.clear();
        for (auto it = threads.begin(); it != threads.end();) {
            if (SuspendThread(it->second) == DWORD(-1)) {  // gone
                CloseHandle(it->second);
                it = threads.erase(it);
                continue;
            }
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            const BOOL got = GetThreadContext(it->second, &c);
            ResumeThread(it->second);
#if defined(_M_ARM64)
            if (got) found.push_back(c.Pc);
#else
            if (got) found.push_back(c.Rip);
#endif
            ++it;
        }
        {
            std::lock_guard l(g_sample_lock);
            ++g_sample_ticks;
            for (uint64_t a : found) ++g_samples[a];
        }
        Sleep(1);
    }
}

bool sampling() {
    static const bool on = [] {
        const char* v = std::getenv("QILIN_SAMPLE");
        return v && *v && *v != '0';
    }();
    return on;
}
#endif

void report_samples() {
#if defined(_WIN32)
    if (!sampling()) return;
    std::unordered_map<uint64_t, uint64_t> samples;
    uint64_t ticks;
    {
        std::lock_guard l(g_sample_lock);
        samples = g_samples, ticks = g_sample_ticks;
    }
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(proc, nullptr, TRUE)) return;
    std::unordered_map<std::string, uint64_t> by_name;
    uint64_t busy = 0, idle = 0;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [addr, n] : samples) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        std::memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO), sym->MaxNameLen = 511;
        DWORD64 off = 0;
        std::string name = SymFromAddr(proc, addr, &off, sym) ? std::string(sym->Name) : std::string("?");
        IMAGEHLP_MODULE64 mod{};
        mod.SizeOfStruct = sizeof mod;
        const std::string module = SymGetModuleInfo64(proc, addr, &mod) ? std::string(mod.ModuleName) : std::string("?");
        // Waiting in the OS: idle, not work.
        const bool waiting = (module == "ntdll" || module == "win32u") &&
                             (name.find("Wait") != std::string::npos || name.find("Delay") != std::string::npos ||
                              name.find("RemoveIoCompletion") != std::string::npos || name.find("Alert") != std::string::npos ||
                              name.find("GetMessage") != std::string::npos);  // the window's thread waiting for events
        if (waiting) {
            idle += n;
            continue;
        }
        busy += n;
        by_name[module + "!" + name] += n;
    }
    std::vector<std::pair<std::string, uint64_t>> top(by_name.begin(), by_name.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "qilin_run: samples: %llu ticks, %.2f threads busy on average (%llu busy samples, %llu waiting)\n",
                 static_cast<unsigned long long>(ticks), ticks ? double(busy) / double(ticks) : 0.0,
                 static_cast<unsigned long long>(busy), static_cast<unsigned long long>(idle));
    for (size_t i = 0; i < top.size() && i < 60; ++i)
        std::fprintf(stderr, "qilin_run:   %6.2f%%  %s\n", busy ? 100.0 * double(top[i].second) / double(busy) : 0.0, top[i].first.c_str());
    SymCleanup(proc);
#endif
}

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

void start_sampling() {
#if defined(_WIN32)
    if (sampling()) std::thread(sample_forever).detach();
#endif
}

void report_profile() {
    report_samples();
    if (!profiling()) return;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const double cpu = cpu_seconds();
    std::fprintf(stderr, "qilin_run: profile: %.1f s of CPU in %.1f s (%.0f%% of one core)\n", cpu, wall,
                 wall > 0 ? 100.0 * cpu / wall : 0.0);
    static const char* const names[] = {"draws (all of each, textures included)", "  of which textures (check, decode)",
                                        "  of which new pipelines (compiles)", "frames (copy, submit, present)", "the mixer"};
    double sections = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(Section::Count); ++i) {
        const double s = double(g_section_ns[i].load()) / 1e9;
        const uint64_t n = g_section_calls[i].load();
        const auto sec = static_cast<Section>(i);
        if (sec != Section::Texture && sec != Section::Pipeline) sections += s;  // those are inside draws
        std::fprintf(stderr, "qilin_run:          %-38s %7.2f s  %9llu calls  %7.1f us each\n", names[i], s,
                     static_cast<unsigned long long>(n), n ? s * 1e6 / double(n) : 0.0);
    }
    std::fprintf(stderr, "qilin_run:          %-38s %7.2f s\n", "the rest (lifted code, OS, waits' CPU)", cpu - sections);
    if (g_gpu_frames)
        std::fprintf(stderr, "qilin_run:          the GPU: %.3f ms a frame on average, %.3f ms at most (%llu frames)\n",
                     double(g_gpu_ns) / double(g_gpu_frames) / 1e6, double(g_gpu_max_ns) / 1e6,
                     static_cast<unsigned long long>(g_gpu_frames));
    std::fprintf(stderr,
                 "qilin_run:          a frame's peak use: %llu KB of vertices, %llu KB of uniforms, %llu KB of texture "
                 "uploads, %llu descriptor sets\n",
                 static_cast<unsigned long long>(g_peak_vertex_bytes >> 10), static_cast<unsigned long long>(g_peak_uniform_bytes >> 10),
                 static_cast<unsigned long long>(g_peak_upload_bytes >> 10), static_cast<unsigned long long>(g_peak_sets));
}

}  // namespace qilin::rt
