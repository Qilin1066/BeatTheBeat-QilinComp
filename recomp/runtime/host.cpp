// The host side on SDL3 (host.hpp): the window and its events, audio out,
// and the keyboard as a Wii Remote. Runtime code: no std::format.
#include "host.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "gx_vulkan.hpp"
#include "input.hpp"
#include "native.hpp"
#include "threads.hpp"

namespace qilin::rt::host {

namespace {

SDL_Window* g_window = nullptr;
SDL_AudioStream* g_audio = nullptr;
std::atomic<uint16_t> g_keys{0};

constexpr uint32_t kSampleRate = 32000;
// Audio queued beyond this is dropped (counted), so the output's latency
// stays bounded if the host falls behind.
constexpr int kMaxQueuedBytes = kSampleRate * 4 / 10;  // 100 ms

// The keyboard as a Wii Remote held upright (docs/runtime.md lists it).
struct Key {
    SDL_Scancode code;
    uint16_t button;
};
constexpr Key kKeys[] = {
    {SDL_SCANCODE_X, kButtonA},         {SDL_SCANCODE_SPACE, kButtonA},   {SDL_SCANCODE_Z, kButtonB},
    {SDL_SCANCODE_LSHIFT, kButtonB},    {SDL_SCANCODE_1, kButton1},       {SDL_SCANCODE_2, kButton2},
    {SDL_SCANCODE_RETURN, kButtonPlus}, {SDL_SCANCODE_BACKSPACE, kButtonMinus}, {SDL_SCANCODE_H, kButtonHome},
    {SDL_SCANCODE_UP, kButtonUp},       {SDL_SCANCODE_DOWN, kButtonDown}, {SDL_SCANCODE_LEFT, kButtonLeft},
    {SDL_SCANCODE_RIGHT, kButtonRight},
};

void update_keys() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    uint16_t held = 0;
    for (const Key& k : kKeys)
        if (state[k.code]) held |= k.button;
    g_keys.store(held, std::memory_order_relaxed);
}

bool open_audio() {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audio) {
        std::fprintf(stderr, "qilin_run: no audio output (%s); running silent\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

}  // namespace

bool windowed() {
    static const bool w = [] {
        const char* v = std::getenv("QILIN_HEADLESS");
        return !(v && *v && *v != '0');
    }();
    return w;
}

uint16_t keyboard_buttons() { return g_keys.load(std::memory_order_relaxed); }

void play_audio(const int16_t* lr, uint32_t frames) {
    if (!g_audio) return;
    if (SDL_GetAudioStreamQueued(g_audio) > kMaxQueuedBytes) {
        ++stats().audio_frames_dropped;
        return;
    }
    SDL_PutAudioStreamData(g_audio, lr, static_cast<int>(frames * 4));
}

bool run(const std::function<void()>& game) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "qilin_run: SDL can't start: %s\n", SDL_GetError());
        return false;
    }
    // The console's 640x480 at twice the size; the frame keeps its aspect in
    // whatever size the window is given.
    g_window = SDL_CreateWindow("Beat the Beat (qilin)", 1280, 720,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        std::fprintf(stderr, "qilin_run: no window: %s\n", SDL_GetError());
        return false;
    }
    if (!gx::init_vulkan(g_window)) return false;
    open_audio();
    std::fprintf(stderr,
                 "qilin_run: keys: A = X or Space, B = Z or Left Shift, 1, 2, + = Enter, - = Backspace, "
                 "Home = H, the D-pad = arrows\n");

    std::thread(game).detach();
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                std::fflush(stderr);
                std::_Exit(0);
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_WINDOW_FOCUS_LOST: update_keys(); break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gx::window_resized(); break;
            default: break;
            }
        } while (SDL_PollEvent(&e));
    }
}

}  // namespace qilin::rt::host
