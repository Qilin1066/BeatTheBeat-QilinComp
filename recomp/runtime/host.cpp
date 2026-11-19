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
std::vector<SDL_Gamepad*> g_pads;  // every connected gamepad: any of them plays the Wii Remote
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

// Controllers as the Wii Remote held upright: the south face button is A,
// the east one and the right trigger B (the remote's trigger), west 1,
// north 2, Start +, Back -, Guide Home; the d-pad and the left stick the
// remote's d-pad.
//
// Every gamepad SDL knows is opened and any of them plays (their buttons
// add up), as the other recomps do it (Zelda64Recomp reads all pads,
// UnleashedRecomp the one last touched). Taking only the first one listed
// went wrong on computers with other game devices: a virtual pad or a wheel
// listed first took the place and the real controller was never read.
uint16_t pad_buttons(SDL_Gamepad* pad) {
    uint16_t held = 0;
    auto button = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    if (button(SDL_GAMEPAD_BUTTON_SOUTH)) held |= kButtonA;
    if (button(SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) held |= kButtonB;
    if (button(SDL_GAMEPAD_BUTTON_WEST)) held |= kButton1;
    if (button(SDL_GAMEPAD_BUTTON_NORTH)) held |= kButton2;
    if (button(SDL_GAMEPAD_BUTTON_START)) held |= kButtonPlus;
    if (button(SDL_GAMEPAD_BUTTON_BACK)) held |= kButtonMinus;
    if (button(SDL_GAMEPAD_BUTTON_GUIDE)) held |= kButtonHome;
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000) held |= kButtonUp;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000) held |= kButtonDown;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000) held |= kButtonLeft;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000) held |= kButtonRight;
    return held;
}

void update_pad() {
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads) held |= pad_buttons(pad);
    g_pad_buttons.store(held, std::memory_order_relaxed);
}

const char* pad_type(SDL_Gamepad* pad) {
    const char* s = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
    return s ? s : "unknown";
}

void attach_pad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (SDL_GetGamepadID(pad) == id) return;  // already open
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        std::fprintf(stderr, "qilin_run: controller: a gamepad can't be opened (%s)\n", SDL_GetError());
        return;
    }
    g_pads.push_back(pad);
    std::fprintf(stderr, "qilin_run: controller: %s (%s, %04X:%04X) plays the Wii Remote\n", SDL_GetGamepadName(pad), pad_type(pad),
                 SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
    update_pad();
}
void detach_pad(SDL_JoystickID id) {
    for (auto it = g_pads.begin(); it != g_pads.end(); ++it)
        if (SDL_GetGamepadID(*it) == id) {
            std::fprintf(stderr, "qilin_run: controller: %s disconnected\n", SDL_GetGamepadName(*it));
            SDL_CloseGamepad(*it);
            g_pads.erase(it);
            break;
        }
    update_pad();
}
// A game device SDL sees but has no gamepad layout for (a wheel, a flight
// stick): said once, so it's known why it does nothing.
void note_joystick(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return;
    const char* name = SDL_GetJoystickNameForID(id);
    std::fprintf(stderr, "qilin_run: controller: %s isn't a gamepad SDL knows the layout of: not used\n", name ? name : "a game device");
}

void apply_rumble() {
    const bool on = g_rumble && settings().rumble;
    for (SDL_Gamepad* pad : g_pads) SDL_RumbleGamepad(pad, on ? 0xA000 : 0, on ? 0xA000 : 0, on ? 60000 : 0);
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

// QILIN_HIDDEN=1: the renderer runs as with a window, but the window isn't
// shown and nothing is played (scripted runs on a computer someone is
// using, with QILIN_FRAME_DUMP to see the frames).
bool hidden() {
    static const bool h = [] {
        const char* v = std::getenv("QILIN_HIDDEN");
        return v && *v && *v != '0';
    }();
    return h;
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
    if (hidden()) flags |= SDL_WINDOW_HIDDEN;
    else if (settings().fullscreen) flags |= SDL_WINDOW_FULLSCREEN;  // borderless, at the desktop's mode
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

// qilin_run --controllers: every game device SDL sees, then each change of
// the Wii Remote's buttons the controllers make, for `seconds`. The same
// code paths as the game's (attach_pad, update_pad), with no game.
bool test_controllers(unsigned seconds) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "qilin_run: SDL can't start: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "qilin_run: SDL %d.%d.%d; looking for controllers for %u s (press buttons)\n", SDL_MAJOR_VERSION,
                 SDL_MINOR_VERSION, SDL_MICRO_VERSION, seconds);
    static constexpr struct {
        uint16_t bit;
        const char* name;
    } kNames[] = {{kButtonA, "A"},       {kButtonB, "B"},         {kButton1, "1"},       {kButton2, "2"},
                  {kButtonPlus, "+"},    {kButtonMinus, "-"},     {kButtonHome, "Home"}, {kButtonUp, "Up"},
                  {kButtonDown, "Down"}, {kButtonLeft, "Left"},   {kButtonRight, "Right"}};
    const uint64_t end = SDL_GetTicks() + 1000ull * seconds;
    uint16_t last = 0;
    unsigned changes = 0;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: attach_pad(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: detach_pad(e.gdevice.which); break;
        case SDL_EVENT_JOYSTICK_ADDED: note_joystick(e.jdevice.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: update_pad(); break;
        default: break;
        }
        const uint16_t now = g_pad_buttons.load(std::memory_order_relaxed);
        if (now != last) {
            ++changes;
            std::fprintf(stderr, "qilin_run: the Wii Remote's buttons held:");
            for (const auto& n : kNames)
                if (now & n.bit) std::fprintf(stderr, " %s", n.name);
            std::fprintf(stderr, "%s\n", now ? "" : " (none)");
            last = now;
        }
    }
    std::fprintf(stderr, "qilin_run: %zu gamepad(s) open, %u button change(s) seen\n", g_pads.size(), changes);
    SDL_Quit();
    return true;
}

bool run(const std::function<void()>& game) {
    SDL_SetHint(SDL_HINT_APP_NAME, kGameName);
    // Controllers are read whether or not the window has the focus (SDL drops
    // their input in the background otherwise: a click elsewhere, an overlay,
    // and the controller seemed dead).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
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
    if (!hidden()) open_audio();
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
            case SDL_EVENT_JOYSTICK_ADDED: note_joystick(e.jdevice.which); break;
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
