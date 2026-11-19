// The console's settings, as the game reads them through SC (the system
// configuration) and VI. On a Wii they are the owner's system settings; here
// they come from the settings file's [console] section (settings.hpp). Compare with Dolphin only with the same settings
// on both sides: they decide 50 or 60 Hz and the video mode.
// Runtime code: no std::format.
#pragma once

#include <cstdint>

#include "settings.hpp"

namespace qilin::rt {

struct ConsoleSettings {
    bool progressive = true;  // 480p, if the "cable" allows it (VIGetDTVStatus)
    bool component = true;    // a cable that can carry 480p
    bool eurgb60 = true;      // 60 Hz on this PAL game (otherwise 50 Hz)
    bool widescreen = true;   // 16:9 (otherwise 4:3)
    // SC's language codes: 0 Japanese, 1 English, 2 German, 3 French,
    // 4 Spanish, 5 Italian, 6 Dutch. This PAL disc has English, French,
    // German, Italian and Spanish.
    uint8_t language = 1;
    uint8_t sound_mode = 1;  // 0 mono, 1 stereo, 2 surround
    // The Wii Remote pointer: the sensor bar's place and its sensitivity (1-5).
    bool sensor_bar_above = false;
    uint8_t dpd_sensitivity = 3;
};

// From the settings file's [console] section (settings.hpp).
inline const ConsoleSettings& console() {
    static const ConsoleSettings s = [] {
        const Settings& f = settings();
        ConsoleSettings c;
        c.progressive = f.progressive, c.eurgb60 = f.sixty_hz, c.widescreen = f.widescreen;
        c.language = f.language, c.sound_mode = f.sound_mode;
        c.sensor_bar_above = f.sensor_bar_above, c.dpd_sensitivity = f.dpd_sensitivity;
        return c;
    }();
    return s;
}

}  // namespace qilin::rt
