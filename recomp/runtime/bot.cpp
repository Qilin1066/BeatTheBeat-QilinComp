// The lockstep test bot's channel (bot.hpp). Runtime code: no std::format.
#include "bot.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "guest.hpp"
#include "native.hpp"

namespace qilin::rt::bot {

void (*at_end)() = nullptr;

namespace {

constexpr uint32_t kVersion = 1, kSampleRate = 32000, kEnd = 0xFFFFFFFFu;

struct Press {
    uint16_t buttons;
    uint64_t from, to;
};

struct State {
    bool checked = false, on = false, greeted = false;
    std::mutex lock;  // the audio interrupt and the frame's end are both the game's, but input reads come from WPAD's
    std::vector<int16_t> audio;  // since the last frame
    uint64_t audio_first = 0, audio_count = 0, audio_started = 0;
    std::vector<Press> presses;
} g;

[[noreturn]] void broken(const char* what) {
    std::fprintf(stderr, "qilin_run: bot: %s; ending the run\n", what);
    std::fflush(stderr);
    std::_Exit(1);
}

void put(const void* p, size_t n) {
    if (std::fwrite(p, 1, n, stdout) != n) broken("its pipe closed");
}
template <class T>
void put(T v) {
    put(&v, sizeof v);  // the hosts are all little-endian (x86-64, ARM64)
}
void get(void* p, size_t n) {
    if (std::fread(p, 1, n, stdin) != n) broken("its pipe closed");
}

}  // namespace

bool active() {
    if (!g.checked) {
        g.checked = true;
        const char* v = std::getenv("QILIN_BOT");
        g.on = v && *v && *v != '0';
        if (g.on) {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::fprintf(stderr, "qilin_run: bot: lockstep on standard input and output\n");
        }
    }
    return g.on;
}

void audio(const int16_t* lr, uint32_t frames, uint64_t started) {
    if (!active()) return;
    std::lock_guard l(g.lock);
    if (g.audio.empty()) g.audio_started = started, g.audio_first = g.audio_count;
    g.audio.insert(g.audio.end(), lr, lr + 2 * frames);
    g.audio_count += frames;
}

void frame(uint64_t number, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!active()) return;
    if (!g.greeted) {
        g.greeted = true;
        put("QBOT", 4);
        put(kVersion), put(uint64_t(kTimeBaseHz)), put(kSampleRate), put(kWidth), put(kHeight);
    }
    // The picture, scaled down by averaging boxes of pixels.
    std::vector<uint8_t> small(size_t(kWidth) * kHeight * 3);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint32_t x0 = x * w / kWidth, x1 = std::max(x0 + 1, (x + 1) * w / kWidth);
            const uint32_t y0 = y * h / kHeight, y1 = std::max(y0 + 1, (y + 1) * h / kHeight);
            uint32_t sum[3] = {}, n = 0;
            for (uint32_t yy = y0; yy < y1 && yy < h; ++yy)
                for (uint32_t xx = x0; xx < x1 && xx < w; ++xx, ++n)
                    for (unsigned c = 0; c < 3; ++c) sum[c] += rgba[4 * (size_t(yy) * w + xx) + c];
            for (unsigned c = 0; c < 3; ++c) small[3 * (size_t(y) * kWidth + x) + c] = uint8_t(n ? sum[c] / n : 0);
        }
    std::vector<int16_t> sound;
    uint64_t first, started;
    {
        std::lock_guard l(g.lock);
        sound.swap(g.audio);
        first = g.audio_first, started = g.audio_started;
    }
    put(number), put(time_base());
    put(first), put(uint32_t(sound.size() / 2)), put(started);
    put(small.data(), small.size());
    if (!sound.empty()) put(sound.data(), sound.size() * sizeof(int16_t));
    if (std::fflush(stdout) != 0) broken("its pipe closed");

    uint32_t count = 0;
    get(&count, sizeof count);
    if (count == kEnd) {
        std::fprintf(stderr, "qilin_run: bot: the bot ended the run\n");
        report_stats();
        if (at_end) at_end();
        std::fflush(stderr);
        std::_Exit(0);
    }
    if (count > 4096) broken("it sent a malformed answer");
    std::vector<Press> presses(count);
    for (Press& p : presses) {
        uint16_t pad = 0;
        get(&p.buttons, 2), get(&pad, 2), get(&p.from, 8), get(&p.to, 8);
        if (p.to < p.from) broken("it sent a press that ends before it starts");
    }
    const uint64_t now = time_base();
    std::lock_guard l(g.lock);
    std::erase_if(g.presses, [now](const Press& p) { return p.to <= now; });  // the ones that are over
    g.presses.insert(g.presses.end(), presses.begin(), presses.end());
}

uint16_t buttons() {
    if (!active()) return 0;
    const uint64_t now = time_base();
    std::lock_guard l(g.lock);
    uint16_t held = 0;
    for (const Press& p : g.presses)
        if (now >= p.from && now < p.to) held |= p.buttons;
    return held;
}

}  // namespace qilin::rt::bot
