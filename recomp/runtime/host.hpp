// The host side (host.cpp, SDL3): a window, audio output and the keyboard.
// Windowing has to live on the process's main thread on some platforms, so
// with a window the game runs on a thread of its own. QILIN_HEADLESS=1 (or a
// QILIN_HEADLESS_ONLY build) runs without any of it, as before.
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <functional>

namespace qilin::rt::host {

#ifdef QILIN_HEADLESS_ONLY
inline bool windowed() { return false; }
inline uint16_t keyboard_buttons() { return 0; }
inline void play_audio(const int16_t*, uint32_t) {}
inline void rumble(bool) {}
#else
// The Wii Remote's rumble, on the controller playing it (if it can).
void rumble(bool on);
// Whether this run has a window, audio and the keyboard.
bool windowed();
// Opens the window, the renderer and audio on the main thread, runs `game`
// on its own host thread, and handles the window's events here until the
// window closes, which ends the process. Returns false (with the reason on
// stderr) if the window or the renderer can't start.
bool run(const std::function<void()>& game);
// The Wii Remote buttons the keyboard and the controller hold (input.hpp's bits).
uint16_t keyboard_buttons();
// Plays stereo 16-bit frames (host byte order, left then right).
void play_audio(const int16_t* lr, uint32_t frames);
#endif

}  // namespace qilin::rt::host
