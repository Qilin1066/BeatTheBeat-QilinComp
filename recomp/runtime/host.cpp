// The host side on SDL3 (host.hpp): the window and its events, audio out,
// and the keyboard and controllers as the Wii Remote. It follows the other
// Qilin recomps: the window is named after the game and opens at the largest
// whole multiple of the picture that fits the display; F11 or Alt+Enter
// switch borderless fullscreen; any controller SDL knows is read through its
// standard layout; the keyboard drives player 1 too, so no controller is
// needed. Runtime code: no std::format.
#include "host.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "bot.hpp"
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
// Opened by the main thread (open_audio, again later if the device couldn't
// be opened at start), used by the game's (play_audio).
std::atomic<SDL_AudioStream*> g_audio{nullptr};
std::vector<SDL_Gamepad*> g_pads;  // every connected gamepad: any of them plays the Wii Remote
std::atomic<uint16_t> g_keys{0}, g_pad_buttons{0};
std::atomic<bool> g_rumble{false};
uint32_t g_wake_event = 0;  // an SDL user event: something for the main thread to do

constexpr uint32_t kSampleRate = 32000;
constexpr int kBytesPerMs = kSampleRate * 4 / 1000;  // 16-bit stereo
// Audio more than this beyond the target (a burst after a stall: the
// periodic interrupts catching up) is dropped, counted, so the lag between
// the game and its sound snaps back at once.
constexpr int kMaxExtraBytes = 40 * kBytesPerMs;

// The audio clock. The game's DMA makes 32 kHz by its own clock (the time
// base); the sound card plays by its crystal. Left alone, the queue between
// them wanders (any lag from 0 to the cap, a different one each session) and
// then clicks as frames are dropped or run out. Instead it is held at the
// target (settings: audio latency_ms): the queue is filled to the target
// before playing starts, then SDL resamples by a ratio that follows the
// queue's average depth (at most 0.5% from the game's speed: inaudible).
// Running dry fills it back to the target with silence at once. Touched only
// from the DMA interrupt (one guest thread at a time) and open_audio.
struct AudioClock {
    bool primed = false;  // the device is playing (the queue reached the target once)
    double depth = 0;     // the queue's depth, smoothed (bytes)
    unsigned frames = 0;  // since the ratio was last set
} g_clock;

bool hidden();

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

// The gamepads already connected, from SDL's list (not only its "added"
// events, which the Ghoulies recomp found it couldn't rely on at start-up).
// attach_pad skips one that is open already.
void attach_connected() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; ++i) attach_pad(ids[i]);
        SDL_free(ids);
    }
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&n)) {
        for (int i = 0; i < n; ++i) note_joystick(ids[i]);
        SDL_free(ids);
    }
}

void apply_rumble() {
    const bool on = g_rumble && settings().rumble;
    for (SDL_Gamepad* pad : g_pads) SDL_RumbleGamepad(pad, on ? 0xA000 : 0, on ? 0xA000 : 0, on ? 60000 : 0);
}
// Opens the default output. `retry`: a later attempt after the device
// couldn't be opened (said only when it works). When Windows can't open the
// device at all (a USB interface's driver stuck: WASAPI's "can't initialize
// audio client", 0x8889000F, found 2026-11-20), every program is silent;
// the main loop tries again every 2 s, so sound comes when the device does.
bool open_audio(bool retry = false) {
    // A small device buffer: the latency is the queue (held at the target)
    // plus this. Backends with a fixed period (WASAPI's shared mode: 10 ms)
    // keep their own.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        // A driver may refuse a buffer that small: its own, then.
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    }
    if (!stream) {
        if (!retry)
            std::fprintf(stderr, "qilin_run: no audio output (%s): Windows or the driver can't open the default device; "
                                 "trying again every 2 s\n", SDL_GetError());
        return false;
    }
    // The device starts paused: it plays once the queue has reached the
    // target (play_audio).
    SDL_AudioSpec out{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &out, &frames) && out.freq > 0)
        std::fprintf(stderr, "qilin_run: audio%s: %d Hz, a %d-sample device buffer (%.1f ms), %u ms queued ahead\n",
                     retry ? " (the device is back)" : "", out.freq, frames, 1000.0 * frames / out.freq, settings().audio_latency_ms);
    g_audio.store(stream, std::memory_order_release);
    return true;
}

// With vsync, the game's clock follows the display when they nearly agree:
// a 60 Hz display shows the game's 59.94 Hz frames with one shown twice
// every 16.7 s (a hitch in anything that scrolls), a 120 Hz one likewise
// every 8.3 s. Running the game at the display's rate (or a whole fraction
// of it) within 0.5% removes that; its sound follows the same clock (the
// audio clock resamples by the same speed). Set once at start: the window's
// display then. Not for runs that must replay alike (hidden, the bot).
double mode_hz(const SDL_DisplayMode& m) {
    return m.refresh_rate_numerator > 0 && m.refresh_rate_denominator > 0
               ? double(m.refresh_rate_numerator) / double(m.refresh_rate_denominator)
               : double(m.refresh_rate);
}
double field_hz() { return console().eurgb60 ? 60000.0 / 1001.0 : 50.0; }
// The game's speed that shows each frame for a whole number of refreshes
// (k) at `hz`, within 0.5%; 0 if there is none.
double lock_speed(double hz, int& k) {
    if (!(hz > 0)) return 0;
    for (k = 1; k <= 4; ++k)
        if (const double speed = hz / k / field_hz(); std::abs(speed - 1.0) <= 0.005) return speed;
    return 0;
}

// The display mode fullscreen switches to (settings: match_refresh), when
// the desktop's refresh can't carry the game's frames evenly (75 Hz, 144 Hz:
// at 75 Hz every fourth frame stays up for an extra refresh, a constant
// judder). The same resolution at a refresh that can (60 or 120 Hz), the
// highest such. F11 and Alt+Enter use it too; windowed play keeps the desktop.
SDL_DisplayMode g_fullscreen_mode{};
bool g_have_fullscreen_mode = false;

void choose_fullscreen_mode() {
    if (hidden() || !settings().vsync || !settings().match_refresh) return;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    int k = 0;
    if (!desktop || lock_speed(mode_hz(*desktop), k) != 0) return;  // the desktop's own refresh suits
    int n = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &n);
    if (!modes) return;
    const SDL_DisplayMode* best = nullptr;
    for (int i = 0; i < n; ++i) {
        const SDL_DisplayMode* m = modes[i];
        if (m->w != desktop->w || m->h != desktop->h || m->pixel_density != desktop->pixel_density) continue;
        if (lock_speed(mode_hz(*m), k) != 0 && (!best || mode_hz(*m) > mode_hz(*best))) best = m;
    }
    if (best) {
        g_fullscreen_mode = *best, g_have_fullscreen_mode = true;
        SDL_SetWindowFullscreenMode(g_window, &g_fullscreen_mode);
        std::fprintf(stderr, "qilin_run: the desktop runs at %.3f Hz; fullscreen switches to %dx%d at %.3f Hz to show every frame evenly\n",
                     mode_hz(*desktop), best->w, best->h, mode_hz(*best));
    } else {
        std::fprintf(stderr, "qilin_run: the desktop runs at %.3f Hz and the display has no %dx%d mode near a multiple of %.2f Hz\n",
                     mode_hz(*desktop), desktop->w, desktop->h, field_hz());
    }
    SDL_free(modes);
}

void lock_to_display() {
    if (hidden() || bot::active() || !settings().vsync || !settings().lock_to_display) return;
    double hz = 0;
    if (g_have_fullscreen_mode && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) {
        hz = mode_hz(g_fullscreen_mode);
    } else if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window))) {
        hz = mode_hz(*m);
    }
    int k = 0;
    if (const double speed = lock_speed(hz, k); speed != 0) {
        set_time_base_speed(speed);
        std::fprintf(stderr, "qilin_run: display %.3f Hz: the game runs at %.3f Hz (%+.2f%%), so every frame is shown %s\n", hz,
                     hz / k, (speed - 1.0) * 100.0, k == 1 ? "once" : k == 2 ? "twice" : k == 3 ? "three times" : "four times");
        return;
    }
    std::fprintf(stderr,
                 "qilin_run: display %.3f Hz: no whole multiple of the game's %.2f Hz is near, so frames are shown unevenly "
                 "(fullscreen%s, a 60 Hz desktop, or a variable-refresh display avoids it)\n",
                 hz, field_hz(), g_have_fullscreen_mode ? " (switches the display to suit)" : "");
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
    SDL_AudioStream* const audio = g_audio.load(std::memory_order_acquire);
    if (!audio) return;
    Stats& st = stats();
    const int target = int(settings().audio_latency_ms) * kBytesPerMs, bytes = int(frames * 4);
    int queued = SDL_GetAudioStreamQueued(audio);
    if (queued < 0) return;  // the device is gone
    if (queued > target + kMaxExtraBytes) {
        ++st.audio_frames_dropped;
        return;
    }
    if (g_clock.primed && queued == 0) {
        // Ran dry (a stall, or the pause for the menu): silence up to the
        // target at once, so the lag is the target again, not whatever the
        // queue grows back to.
        ++st.audio_underruns;
        static const std::vector<uint8_t> silence(size_t(200 + 40) * kBytesPerMs, 0);
        const int fill = std::max(target - bytes, 0);
        SDL_PutAudioStreamData(audio, silence.data(), fill & ~3);
        queued = fill & ~3;
    }
    SDL_PutAudioStreamData(audio, lr, bytes);
    queued += bytes;
    if (!g_clock.primed) {
        if (queued < target) return;
        g_clock.primed = true, g_clock.depth = queued, g_clock.frames = 0;
        SDL_ResumeAudioStreamDevice(audio);
        return;
    }
    st.audio_queue_sum += uint64_t(queued), ++st.audio_queue_count;
    st.audio_queue_min = std::min<uint64_t>(st.audio_queue_min, uint64_t(queued));
    st.audio_queue_max = std::max<uint64_t>(st.audio_queue_max, uint64_t(queued));
    // The depth over about 200 ms (64 frames of 3 ms): the device takes the
    // queue in its own bursts, so one reading says little.
    g_clock.depth += (queued - g_clock.depth) / 64.0;
    if (++g_clock.frames < 16) return;  // the ratio every 48 ms
    g_clock.frames = 0;
    // Proportional: 10 ms too deep plays 1/3% faster at a 30 ms target, which
    // takes it back in about 3 s. The clocks' own difference (a few in ten
    // thousand) leaves a steady error of about a millisecond.
    const double error = (g_clock.depth - target) / target;
    const double ratio = time_base_speed() * (1.0 + std::clamp(error * 0.01, -0.005, 0.005));
    st.audio_ratio = ratio;
    SDL_SetAudioStreamFrequencyRatio(audio, float(ratio));
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
    attach_connected();
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
    attach_connected();
    std::fprintf(stderr,
                 "qilin_run: settings: %s (preset %s, %ux internal resolution, %ux anisotropic filtering)\n"
                 "qilin_run: keys: A = Space, K or X; B = L, Z or Left Shift; 1 = 1 or J; 2 = 2 or I; + = Enter; "
                 "- = Backspace or Tab; Home = H; the d-pad = arrows; F11 or Alt+Enter: fullscreen\n",
                 settings_path().string().c_str(), settings().preset.c_str(), settings().internal_scale, settings().anisotropy);

    choose_fullscreen_mode();
    lock_to_display();  // before the game's threads: the clock's speed is set once
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
        // No sound device yet (it couldn't be opened): try again every 2 s.
        if (!hidden() && !g_audio.load(std::memory_order_relaxed)) {
            static uint64_t next_try = 0;
            if (const uint64_t now = SDL_GetTicks(); now >= next_try) {
                if (next_try) open_audio(true);
                next_try = now + 2000;
            }
        }
        if (was_shown != menu.is_open()) {  // opened or closed: what's held now is the game's again
            update_keys();
            update_pad();
            apply_rumble();
        }
    }
}
}  // namespace qilin::rt::host
