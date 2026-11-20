// Pausing the game for the settings menu (pause.cpp). The game's thread parks
// at the end of a frame, the one point where the renderer has nothing in
// hand, so the main thread can draw the menu with it. Only one guest thread
// runs at a time and the parked one holds the run token, so the whole game
// stands still; the time base stops too, so no time passes for it (its
// clock, alarms and the audio clock go on from where they were).
// Runtime code: no std::format.
#pragma once

namespace qilin::rt {

// The main thread: ask for the pause, or end it.
void request_pause(bool on);
// The main thread: whether the game has parked (the renderer is the main
// thread's until the pause ends).
bool game_parked();
// The game's thread, at the end of each frame: parks while a pause is asked for.
void park_if_requested();
// Whether a pause is asked for (the renderer drains its queue before parking).
bool pause_requested();

}  // namespace qilin::rt
