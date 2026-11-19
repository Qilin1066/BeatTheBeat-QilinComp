// A test bot driving the game in lockstep (bot.cpp), for runs no fixed
// script can do: QILIN_BOT=1, with the bot on the other end of the process's
// standard input and output (tools/bot/qilinbot.py starts it so).
//
// At the end of every frame the runtime sends the bot what the frame showed
// and what was heard since the last one, and waits for its answer: the
// presses to make, each from one moment of the game's clock to another. The
// game's clock stops while it waits (the settings menu's pause), so the bot
// can take as long as it needs and a busy computer changes nothing the game
// sees. Presses are timed in the game's own clock (time-base ticks), so they
// land inside a frame where the bot wants them, not on frame edges.
//
// The protocol, little-endian. Runtime to bot, once: "QBOT", version (u32),
// time-base ticks a second (u64), audio sample rate (u32), picture width and
// height (u32 each). Then each frame:
//   frame number (u64), the game's clock at the frame's end (u64),
//   audio: the first sample's number (u64), how many stereo samples (u32),
//          the game's clock when that first sample started playing (u64),
//   the picture: width x height RGB bytes (the console's picture, scaled
//          down), then the samples (L, R as i16).
// Bot to runtime, after each frame: a count (u32), then that many presses:
//   buttons (u16, the remote's bits: input.hpp), two padding bytes, from and
//   to (u64 each, the game's clock). A count of 0xFFFFFFFF ends the run
//   (exit status 0). Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace qilin::rt::bot {

bool active();
// Called when the bot ends the run, after the counters (the game's main
// program sets it to the thread report).
extern void (*at_end)();
// The picture size sent (the console's 640x456 picture scaled down 4 times).
inline constexpr uint32_t kWidth = 160, kHeight = 114;
// Audio as AI plays it (the DMA interrupt): a frame of stereo samples, and
// the game's clock when it started playing.
void audio(const int16_t* lr, uint32_t frames, uint64_t started);
// The frame's end (the game's thread; the renderer has the picture as RGBA
// rows of w x h): exchange with the bot, the game's clock stopped meanwhile.
void frame(uint64_t number, const uint8_t* rgba, uint32_t w, uint32_t h);
// The buttons the bot holds now (the game's clock).
uint16_t buttons();

}  // namespace qilin::rt::bot
