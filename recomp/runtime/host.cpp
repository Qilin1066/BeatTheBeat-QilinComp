// The host side on SDL3 (host.hpp): the window and its events, audio out,
// and the keyboard and controllers as the Wii Remote. It follows the other
// Qilin recomps: the window is named after the game and opens at the largest
// whole multiple of the picture that fits the display; F11 or Alt+Enter
// switch borderless fullscreen; any controller SDL knows is read through its
// standard layout; the keyboard drives player 1 too, so no controller is
// needed. Runtime code: no std::format.
#include "host.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "console.hpp"
#include "gx_vulkan.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "native.hpp"
#include "settings.hpp"

namespace qilin::rt::host {

namespace {

constexpr const char* kGameName = "Beat the Beat: Rhythm Paradise";

SDL_Window* g_window = nullptr;
SDL_AudioStream* g_audio = nullptr;
SDL_Gamepad* g_pad = nullptr;  // the controller playing as the Wii Remote (the first connected)
std::atomic<uint16_t> g_keys{0}, g_pad_buttons{0};
std::atomic<bool> g_rumble{false};
uint32_t g_wake_event = 0;  // an SDL user event: something for the main thread to do

constexpr uint32_t kSampleRate = 32000;
// Audio queued beyond this is dropped (counted), so the output's latency
// stays bounded if the host falls behind.
constexpr int kMaxQueuedBytes = kSampleRate * 4 / 10;  // 100 ms

// The keyboard as a Wii Remote held upright. The other Qilin recomps' keys
// (Space or K for the main button, L for the second, Enter Start, Backspace
// or Tab Back) and this one's first ones (X, Z) both work.
struct Key {
    SDL_Scancode code;
    uint16_t button;
};
constexpr Key kKeys[] = {
    {SDL_SCANCODE_SPACE, kButtonA},  {SDL_SCANCODE_K, kButtonA},         {SDL_SCANCODE_X, kButtonA},
    {SDL_SCANCODE_L, kButtonB},      {SDL_SCANCODE_Z, kButtonB},         {SDL_SCANCODE_LSHIFT, kButtonB},
    {SDL_SCANCODE_1, kButton1},      {SDL_SCANCODE_J, kButton1},         {SDL_SCANCODE_2, kButton2},
    {SDL_SCANCODE_I, kButton2},      {SDL_SCANCODE_RETURN, kButtonPlus}, {SDL_SCANCODE_BACKSPACE, kButtonMinus},
    {SDL_SCANCODE_TAB, kButtonMinus}, {SDL_SCANCODE_H, kButtonHome},     {SDL_SCANCODE_UP, kButtonUp},
    {SDL_SCANCODE_DOWN, kButtonDown}, {SDL_SCANCODE_LEFT, kButtonLeft},  {SDL_SCANCODE_RIGHT, kButtonRight},
};

void update_keys() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    uint16_t held = 0;
    for (const Key& k : kKeys)
        if (state[k.code]) held |= k.button;
    g_keys.store(held, std::memory_order_relaxed);
}

// A controller as the Wii Remote held upright: the south face button is A,
// the east one and the right trigger B (the remote's trigger), west 1,
// north 2, Start +, Back -, Guide Home; the d-pad and the left stick the
// remote's d-pad.
void update_pad() {
    uint16_t held = 0;
    if (g_pad) {
        auto button = [](SDL_GamepadButton b) { return SDL_GetGamepadButton(g_pad, b); };
        if (button(SDL_GAMEPAD_BUTTON_SOUTH)) held |= kButtonA;
        if (button(SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) held |= kButtonB;
        if (button(SDL_GAMEPAD_BUTTON_WEST)) held |= kButton1;
        if (button(SDL_GAMEPAD_BUTTON_NORTH)) held |= kButton2;
        if (button(SDL_GAMEPAD_BUTTON_START)) held |= kButtonPlus;
        if (button(SDL_GAMEPAD_BUTTON_BACK)) held |= kButtonMinus;
        if (button(SDL_GAMEPAD_BUTTON_GUIDE)) held |= kButtonHome;
        const int x = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000) held |= kButtonUp;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000) held |= kButtonDown;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000) held |= kButtonLeft;
        if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000) held |= kButtonRight;
    }
    g_pad_buttons.store(held, std::memory_order_relaxed);
}

void attach_pad(SDL_JoystickID id) {
    if (g_pad) return;  // one Wii Remote: the first controller plays it
    g_pad = SDL_OpenGamepad(id);
    if (g_pad) std::fprintf(stderr, "qilin_run: controller: %s plays the Wii Remote\n", SDL_GetGamepadName(g_pad));
    update_pad();
}
void detach_pad(SDL_JoystickID id) {
    if (!g_pad || SDL_GetGamepadID(g_pad) != id) return;
    SDL_CloseGamepad(g_pad);
    g_pad = nullptr;
    // Another controller still connected takes over.
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        if (n > 0) attach_pad(ids[0]);
        SDL_free(ids);
    }
    update_pad();
}

void apply_rumble() {
    const bool on = g_rumble && settings().rumble;
    if (g_pad) SDL_RumbleGamepad(g_pad, on ? 0xA000 : 0, on ? 0xA000 : 0, on ? 60000 : 0);
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

// The window: the largest whole multiple of the picture (640x480 at 4:3,
// 854x480 at 16:9) that fits nine tenths of the settings' display's usable
// area, centred on it.
SDL_Window* open_window() {
    const uint32_t width = console().widescreen ? 854 : 640, height = 480;
    uint32_t scale = 1;
    SDL_Rect usable{};
    const SDL_DisplayID display = chosen_display();
    if (SDL_GetDisplayUsableBounds(display, &usable))
        while (width * (scale + 1) <= uint32_t(usable.w) * 9 / 10 && height * (scale + 1) <= uint32_t(usable.h) * 9 / 10) ++scale;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (settings().fullscreen) flags |= SDL_WINDOW_FULLSCREEN;  // borderless, at the desktop's mode
    SDL_Window* w = SDL_CreateWindow(kGameName, int(width * scale), int(height * scale), flags);
    if (w) {
        SDL_SetWindowMinimumSize(w, 160, 120);
        const int at = int(SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        SDL_SetWindowPosition(w, at, at);
    }
    return w;
}

}  // namespace

bool windowed() {
    static const bool w = [] {
        const char* v = std::getenv("QILIN_HEADLESS");
        return !(v && *v && *v != '0');
    }();
    return w;
}

uint16_t keyboard_buttons() {
    return g_keys.load(std::memory_order_relaxed) | g_pad_buttons.load(std::memory_order_relaxed);
}

void play_audio(const int16_t* lr, uint32_t frames) {
    if (!g_audio) return;
    if (SDL_GetAudioStreamQueued(g_audio) > kMaxQueuedBytes) {
        ++stats().audio_frames_dropped;
        return;
    }
    SDL_PutAudioStreamData(g_audio, lr, static_cast<int>(frames * 4));
}

void rumble(bool on) {
    if (g_rumble.exchange(on) == on || !g_wake_event) return;
    SDL_Event e{};
    e.type = g_wake_event;  // the main thread applies it (SDL_PushEvent is thread-safe)
    SDL_PushEvent(&e);
}

bool run(const std::function<void()>& game) {
    SDL_SetHint(SDL_HINT_APP_NAME, kGameName);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "qilin_run: SDL can't start: %s\n", SDL_GetError());
        return false;
    }
    g_wake_event = SDL_RegisterEvents(1);
    g_window = open_window();
    if (!g_window) {
        std::fprintf(stderr, "qilin_run: no window: %s\n", SDL_GetError());
        return false;
    }
    if (!gx::init_vulkan(g_window)) return false;
    open_audio();
    std::fprintf(stderr,
                 "qilin_run: settings: %s (preset %s, %ux internal resolution, %ux anisotropic filtering)\n"
                 "qilin_run: keys: A = Space, K or X; B = L, Z or Left Shift; 1 = 1 or J; 2 = 2 or I; + = Enter; "
                 "- = Backspace or Tab; Home = H; the d-pad = arrows; F11 or Alt+Enter: fullscreen\n",
                 settings_path().string().c_str(), settings().preset.c_str(), settings().internal_scale, settings().anisotropy);

    std::thread(game).detach();
    Menu menu(g_window);
    // QILIN_MENU_TOGGLE=<seconds>,...: the menu opened or closed at those
    // times after start (scripted runs: the pause, the overlay).
    std::vector<uint64_t> toggles;
    if (const char* v = std::getenv("QILIN_MENU_TOGGLE"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const double s = std::strtod(p, &end);
            if (end == p) {
                std::fprintf(stderr, "qilin_run: QILIN_MENU_TOGGLE is seconds, comma-separated\n");
                return false;
            }
            toggles.push_back(SDL_GetTicks() + uint64_t(s * 1000));
            p = *end == ',' ? end + 1 : end;
        }
    auto quit = [] {
        std::fflush(stderr);
        std::_Exit(0);
    };
    for (;;) {
        SDL_Event e;
        // The menu draws a frame each pass while it's up (vsync paces it).
        bool got = SDL_WaitEventTimeout(&e, menu.busy() ? 10 : 100);
        const bool was_shown = menu.is_open();
        while (got) {
            bool taken = false;
            try {  // F1 or Esc closing the menu applies the settings
                taken = menu.handle(e);
            } catch (const Stop& s) {
                report_stop(s);
                std::fflush(stderr);
                std::_Exit(1);
            }
            if (taken) {
                got = SDL_PollEvent(&e);
                continue;
            }
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (menu.is_open()) save_settings();
                quit();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                    const bool full = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(g_window, full);
                    // Kept for the next start. With the game running, only
                    // the main thread writes settings, and the game doesn't
                    // read this one; the open menu saves when it closes.
                    settings().fullscreen = full;
                    if (!menu.is_open()) save_settings();
                    break;  // Alt+Enter isn't the remote's +
                }
                update_keys();
                break;
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_WINDOW_FOCUS_LOST: update_keys(); break;
            case SDL_EVENT_GAMEPAD_ADDED: attach_pad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: detach_pad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: update_pad(); break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gx::window_resized(); break;
            default:
                if (e.type == g_wake_event) apply_rumble();
                break;
            }
            got = SDL_PollEvent(&e);
        }
        try {  // the renderer can stop on this thread too while the menu has it
            if (!toggles.empty() && SDL_GetTicks() >= toggles.front()) {
                toggles.erase(toggles.begin());
                menu.toggle();
            }
            menu.tick();
        } catch (const Stop& s) {
            report_stop(s);
            std::fflush(stderr);
            std::_Exit(1);
        }
        if (menu.quit_requested()) quit();
        if (was_shown != menu.is_open()) {  // opened or closed: what's held now is the game's again
            update_keys();
            update_pad();
            apply_rumble();
        }
    }
}
}  // namespace qilin::rt::host
