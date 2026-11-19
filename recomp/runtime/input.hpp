// Where the Wii Remote's buttons come from (input.cpp). Runtime code: no
// std::format.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::rt {

// WPAD's core button bits, the remote's own report layout.
inline constexpr uint16_t kButtonLeft = 0x0001, kButtonRight = 0x0002, kButtonDown = 0x0004, kButtonUp = 0x0008,
                          kButtonPlus = 0x0010, kButton2 = 0x0100, kButton1 = 0x0200, kButtonB = 0x0400,
                          kButtonA = 0x0800, kButtonMinus = 0x1000, kButtonHome = 0x8000;

// A QILIN_INPUT script (input.cpp's comment has the syntax): presses held
// from `from` until `to`, in seconds after the runtime starts. False, with
// the part it can't read in `bad`, for a malformed script.
struct ScriptedPress {
    double from, to;
    uint16_t buttons;
};
bool parse_input_script(std::string_view script, std::vector<ScriptedPress>& out, std::string& bad);

// Whether there is an input source, so a remote should connect.
bool input_source();
// The buttons held now on a channel.
uint16_t input_buttons(unsigned channel);

}  // namespace qilin::rt
