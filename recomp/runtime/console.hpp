// The console's settings, as the game reads them through SC (the system
// configuration) and VI. On a Wii they are the owner's system settings; here
// they are the runtime's, until the settings file exists (roadmap: settings,
// not platform branches). Compare with Dolphin only with the same settings
// on both sides: they decide 50 or 60 Hz and the video mode.
// Runtime code: no std::format.
#pragma once

namespace qilin::rt {

struct ConsoleSettings {
    bool progressive = true;  // 480p, if the "cable" allows it (VIGetDTVStatus)
    bool component = true;    // a cable that can carry 480p
    bool eurgb60 = true;      // 60 Hz on this PAL game (otherwise 50 Hz)
    bool widescreen = true;   // 16:9 (otherwise 4:3)
};

inline const ConsoleSettings& console() {
    static const ConsoleSettings s;
    return s;
}

}  // namespace qilin::rt
