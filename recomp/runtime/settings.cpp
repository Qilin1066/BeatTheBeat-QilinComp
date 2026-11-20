// The settings file (settings.hpp). Runtime code: no std::format.
#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace qilin::rt {

std::filesystem::path user_data_base() {
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA"); p && *p) return p;
#elif defined(__APPLE__)
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / "Library" / "Application Support";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return p;
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / ".local" / "share";
#endif
    return {};
}

// The presets: what each sets before the file's own keys.
bool apply_preset(Settings& s, const std::string& name) {
    if (name == "original") {  // the console's picture: 1x, its filtering, vsync
        s.internal_scale = 1, s.anisotropy = 1, s.vsync = true;
    } else if (name == "low-end") {  // a Raspberry Pi 4-class board: the console's work, nothing more
        s.internal_scale = 1, s.anisotropy = 1, s.vsync = true;
    } else if (name == "desktop") {  // a desktop GPU: 3x (1920x1584 frame buffer), 16x anisotropic
        s.internal_scale = 3, s.anisotropy = 16, s.vsync = true;
    } else {
        return false;
    }
    s.preset = name;
    return true;
}

// The file, with its comments, for the values in `s` (the first run's
// default file, and what the menu saves).
std::string settings_text(const Settings& s) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t;
    auto line = [&](const std::string& l) { t += l + "\n"; };
    line("# Beat the Beat: Rhythm Paradise - settings");
    line("#");
    line("# The settings menu (F1 or Esc in the game) changes these and saves them here.");
    line("# A preset sets the enhancements; any key below it overrides one.");
    line("# Delete this file to get the defaults back.");
    line("");
    line("[display]");
    line("# original: the console's picture. low-end: for a Raspberry Pi 4-class board.");
    line("# desktop: 3x internal resolution, 16x anisotropic filtering.");
    line("preset = " + s.preset);
    line("# Borderless fullscreen at the desktop's resolution (F11 or Alt+Enter switch it).");
    line(std::string("fullscreen = ") + b(s.fullscreen));
    line("# The display, by name (empty: the main one).");
    line("monitor = " + s.monitor);
    line("# The picture's size: integer (whole multiples, the sharpest), fit (keeping its shape), stretch (the whole window).");
    line(std::string("scaling = ") + scaling_name(s.scaling));
    line("# Smooth (filtered) at sizes that aren't whole multiples; false: sharp.");
    line(std::string("smooth = ") + b(s.smooth));
    line("# The frame buffer at 1-8x the console's 640x528 (more is sharper, and more work for the GPU).");
    line("internal_scale = " + std::to_string(s.internal_scale));
    line("# Texture filtering: 1 (the console's), 2, 4, 8 or 16.");
    line("anisotropy = " + std::to_string(s.anisotropy));
    line("# Present at the display's refresh (no tearing). false: the lowest latency.");
    line(std::string("vsync = ") + b(s.vsync));
    line("# The graphics device, by name (empty: the best one).");
    line("gpu = " + s.gpu);
    line("");
    line("[controls]");
    line("# The Wii Remote's rumble on the controller.");
    line(std::string("rumble = ") + b(s.rumble));
    line("");
    line("[console]");
    line("# What the game reads as the Wii's own settings (read when the game starts).");
    line("# 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian (this disc: 1-5).");
    line("language = " + std::to_string(s.language));
    line("# 60 Hz (EuRGB60); false: PAL 50 Hz.");
    line(std::string("sixty_hz = ") + b(s.sixty_hz));
    line("# 16:9; false: 4:3.");
    line(std::string("widescreen = ") + b(s.widescreen));
    line(std::string("progressive = ") + b(s.progressive));
    line("# 0 mono, 1 stereo, 2 surround (the game encodes Dolby Pro Logic II, for a receiver that decodes it).");
    line("sound_mode = " + std::to_string(s.sound_mode));
    line(std::string("sensor_bar_above = ") + b(s.sensor_bar_above));
    line("# Pointer sensitivity, 1-5.");
    line("dpd_sensitivity = " + std::to_string(s.dpd_sensitivity));
    return t;
}

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Settings load() {
    Settings s;
    const std::filesystem::path path = settings_path();
    if (path.empty()) return s;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << settings_text(Settings{});
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return s;
    std::string error;
    unsigned line = 0;
    if (!parse_settings(f, s, error, line)) {
        std::fprintf(stderr, "qilin_run: %s, line %u: %s\n", path.string().c_str(), line, error.c_str());
        std::fflush(stderr);
        std::_Exit(2);
    }
    return s;
}
}  // namespace

bool parse_settings(std::istream& in, Settings& s, std::string& error, unsigned& line_number) {
    std::string line, section;
    for (unsigned n = 1; std::getline(in, line); ++n) {
        line_number = n;
        auto bad = [&](const std::string& what) {
            error = what;
            return false;
        };
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return bad("a section is written [name]");
            section = trim(line.substr(1, line.size() - 2));
            if (section != "display" && section != "controls" && section != "console") return bad("no section [" + section + "]");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return bad("a setting is written key = value");
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        bool ok = true;
        auto boolean = [&](bool& out) {
            if (value == "true") out = true;
            else if (value == "false") out = false;
            else ok = bad(key + " is true or false");
        };
        auto number = [&](uint32_t lo, uint32_t hi) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (value.empty() || *end || value.front() == '-' || v < lo || v > hi) {
                ok = bad(key + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
                return lo;
            }
            return static_cast<uint32_t>(v);
        };
        if (section == "display" && key == "preset") {
            if (!apply_preset(s, value)) return bad("the presets are original, low-end and desktop");
        } else if (section == "display" && key == "fullscreen") {
            boolean(s.fullscreen);
        } else if (section == "display" && key == "internal_scale") {
            s.internal_scale = number(1, 8);
        } else if (section == "display" && key == "anisotropy") {
            s.anisotropy = number(1, 16);
            if (ok && (s.anisotropy & (s.anisotropy - 1))) return bad("anisotropy is 1, 2, 4, 8 or 16");
        } else if (section == "display" && key == "monitor") {
            s.monitor = value;
        } else if (section == "display" && key == "gpu") {
            s.gpu = value;
        } else if (section == "display" && key == "scaling") {
            if (value == "integer") s.scaling = Scaling::integer;
            else if (value == "fit") s.scaling = Scaling::fit;
            else if (value == "stretch") s.scaling = Scaling::stretch;
            else return bad("scaling is integer, fit or stretch");
        } else if (section == "display" && key == "smooth") {
            boolean(s.smooth);
        } else if (section == "controls" && key == "rumble") {
            boolean(s.rumble);
        } else if (section == "display" && key == "vsync") {
            boolean(s.vsync);
        } else if (section == "console" && key == "language") {
            s.language = static_cast<uint8_t>(number(0, 6));
        } else if (section == "console" && key == "sixty_hz") {
            boolean(s.sixty_hz);
        } else if (section == "console" && key == "widescreen") {
            boolean(s.widescreen);
        } else if (section == "console" && key == "progressive") {
            boolean(s.progressive);
        } else if (section == "console" && key == "sound_mode") {
            s.sound_mode = static_cast<uint8_t>(number(0, 2));
        } else if (section == "console" && key == "sensor_bar_above") {
            boolean(s.sensor_bar_above);
        } else if (section == "console" && key == "dpd_sensitivity") {
            s.dpd_sensitivity = static_cast<uint8_t>(number(1, 5));
        } else {
            return bad("no setting " + (section.empty() ? key : "[" + section + "] " + key));
        }
        if (!ok) return false;
    }
    return true;
}
std::filesystem::path game_data_dir() {
    static const std::filesystem::path dir = [] {
        const std::filesystem::path base = user_data_base();
        if (base.empty()) return base;
        const std::filesystem::path now = base / "qilin" / "beatthebeat", before = base / "BeatTheBeat-QilinComp";
        // Up to v0.16.0 the folder was named after the project, which was
        // then called BeatTheBeat-QilinComp (the old name stays here: it's
        // the folder's). What's there (saves above all) moves to the new
        // place once.
        std::error_code ec;
        if (std::filesystem::is_directory(before, ec) && !std::filesystem::exists(now, ec)) {
            std::filesystem::create_directories(now.parent_path(), ec);
            std::filesystem::rename(before, now, ec);
            if (ec) std::fprintf(stderr, "qilin_run: can't move %s to %s (%s): your saves are still in the old place\n",
                                 before.string().c_str(), now.string().c_str(), ec.message().c_str());
            else std::fprintf(stderr, "qilin_run: moved %s to %s\n", before.string().c_str(), now.string().c_str());
        }
        return now;
    }();
    return dir;
}

std::filesystem::path settings_path() {
    if (const char* p = std::getenv("QILIN_SETTINGS"); p && *p) return p;
    const std::filesystem::path dir = game_data_dir();
    return dir.empty() ? dir : dir / "beatthebeat.ini";
}

Settings& settings() {
    static Settings s = load();
    return s;
}

bool save_settings() {
    const std::filesystem::path path = settings_path();
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << settings_text(settings());
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

const char* scaling_name(Scaling s) {
    switch (s) {
    case Scaling::integer: return "integer";
    case Scaling::stretch: return "stretch";
    default: return "fit";
    }
}

}  // namespace qilin::rt
