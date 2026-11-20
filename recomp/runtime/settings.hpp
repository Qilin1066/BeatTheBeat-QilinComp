// The settings file (settings.cpp): enhancements and the console's own
// settings, with presets, as in the other Qilin recomps ("settings, not
// platform branches": docs/roadmap-and-design-targets.md).
//
//   <the game's data folder>/beatthebeat.ini   (QILIN_SETTINGS=<file> instead)
//
// Written with every key, commented, on the first run. A preset sets the
// enhancements' defaults; any key after it overrides one. A key that isn't
// known or a value that doesn't fit stops the runtime at start, naming the
// file and line (nothing is guessed). Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>

namespace qilin::rt {

// How the picture fills the window.
enum class Scaling : uint8_t { integer, fit, stretch };  // whole multiples; the largest that keeps its shape; the whole window

struct Settings {
    // [display]
    std::string preset = "original";  // original, low-end, desktop
    bool fullscreen = false;          // borderless, at the desktop's mode (F11 or Alt+Enter switch it)
    std::string monitor;              // the display's name (empty: the main one)
    Scaling scaling = Scaling::fit;   // the picture's size in the window
    bool smooth = true;               // filtered at sizes that aren't whole multiples
    uint32_t internal_scale = 1;      // the frame buffer at 1-8x the console's 640x528
    uint32_t anisotropy = 1;          // texture filtering: 1 (the console's), 2, 4, 8 or 16
    bool vsync = true;                // present at the display's refresh (no tearing); off: lowest latency
    bool lock_to_display = true;      // with vsync: the game's clock at the display's refresh when within 0.5% (read at start)
    bool match_refresh = true;        // with vsync: fullscreen switches to a refresh that suits the game (60/120 Hz) if the desktop's doesn't
    bool variable_refresh = false;    // G-Sync/FreeSync/VRR on: the display follows the game's frames (no lock, no mode switch; read at start)
    std::string gpu;                  // the graphics device's name (empty: the best one)
    // [audio]
    uint32_t audio_latency_ms = 10;   // the sound queued ahead of the speakers, held there (5-200; at least what the device can keep up)
    bool exclusive_audio = true;      // Windows: the device for the game alone (WASAPI exclusive: the shortest periods)
    // [controls]
    bool rumble = true;  // the Wii Remote's rumble on the controller
    // [console]: what the game reads as the Wii's own settings
    uint8_t language = 1;          // 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian, 6 Dutch
    bool sixty_hz = true;          // EuRGB60 (60 Hz) rather than PAL 50 Hz
    bool widescreen = true;        // 16:9 rather than 4:3
    bool progressive = true;       // 480p
    uint8_t sound_mode = 1;        // 0 mono, 1 stereo, 2 surround
    bool sensor_bar_above = false; // the Wii Remote's sensor bar above the TV
    uint8_t dpd_sensitivity = 3;   // pointer sensitivity, 1-5
};

// The settings, read once (the first call reads the file, or writes the
// default one); a bad file ends the process with the reason. The menu
// changes them (on the main thread, while the game is paused) and saves.
Settings& settings();
// Writes the settings out (the file's comments, the current values); false
// if it can't.
bool save_settings();
// The file's text, with its comments, for these values (what save writes).
std::string settings_text(const Settings& s);
const char* scaling_name(Scaling s);
// Sets a preset's enhancements (and its name); false for an unknown one.
bool apply_preset(Settings& s, const std::string& name);
// Where the file is.
std::filesystem::path settings_path();
// The game's data folder: saves, the settings file, the pipeline cache.
// <per-user data>/qilin/beatthebeat, as the other Qilin recomps keep theirs.
std::filesystem::path game_data_dir();
// The platform's per-user application data (%LOCALAPPDATA%, ~/Library/
// Application Support, $XDG_DATA_HOME or ~/.local/share); empty if none.
std::filesystem::path user_data_base();
// Reads settings from `in` over `s` (a preset first sets the
// enhancements). False with the reason and the line for a malformed one.
bool parse_settings(std::istream& in, Settings& s, std::string& error, unsigned& line);

}  // namespace qilin::rt
