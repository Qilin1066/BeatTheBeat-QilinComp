// Input sources for the Wii Remote (input.hpp): the keyboard when there is a
// window (host.cpp), and QILIN_INPUT, a script of button presses for
// headless and repeatable runs (both at once add up), e.g.
//
//   QILIN_INPUT=A@5,A@8.5,B+A@12:0.3
//
// presses A 5 s and 8.5 s after the runtime starts, then B and A together
// at 12 s for 0.3 s. A press lasts 0.1 s unless given. Buttons: A B 1 2
// PLUS MINUS HOME UP DOWN LEFT RIGHT. A malformed script stops the runtime
// at start. Runtime code: no std::format.
#include "input.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "guest.hpp"
#include "host.hpp"

namespace qilin::rt {

namespace {

uint16_t button(std::string_view name) {
    struct Name {
        std::string_view name;
        uint16_t bit;
    };
    static constexpr Name names[] = {{"A", kButtonA},       {"B", kButtonB},       {"1", kButton1},
                                     {"2", kButton2},       {"PLUS", kButtonPlus}, {"MINUS", kButtonMinus},
                                     {"HOME", kButtonHome}, {"UP", kButtonUp},     {"DOWN", kButtonDown},
                                     {"LEFT", kButtonLeft}, {"RIGHT", kButtonRight}};
    for (const Name& n : names)
        if (name == n.name) return n.bit;
    return 0;
}

// The parts of `s` between commas (or pluses), empty ones included.
template <class F>
void split(std::string_view s, char sep, F&& part) {
    for (size_t i = 0;;) {
        const size_t j = s.find(sep, i);
        part(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        if (j == std::string_view::npos) return;
        i = j + 1;
    }
}

struct Script {
    bool parsed = false, present = false;
    std::vector<ScriptedPress> presses;
} g_script;

// Press times count from the runtime's start: this initialiser runs before main.
const uint64_t g_start = time_base();

void load() {
    if (g_script.parsed) return;
    g_script.parsed = true;
    const char* v = std::getenv("QILIN_INPUT");
    if (!v || !*v) return;
    std::string bad;
    if (!parse_input_script(v, g_script.presses, bad)) {
        std::fprintf(stderr, "qilin_run: QILIN_INPUT: can't read \"%s\" (BUTTON[+BUTTON]@SECONDS[:DURATION])\n", bad.c_str());
        std::fflush(stderr);
        std::_Exit(2);
    }
    g_script.present = true;
}

}  // namespace

bool parse_input_script(std::string_view script, std::vector<ScriptedPress>& out, std::string& bad) {
    out.clear();
    bool ok = true;
    split(script, ',', [&](std::string_view part) {
        if (!ok || part.empty()) return;
        const size_t at = part.find('@');
        uint16_t buttons = 0;
        if (at != std::string_view::npos)
            split(part.substr(0, at), '+', [&](std::string_view name) {
                const uint16_t b = button(name);
                buttons = b && buttons != 0xFFFF ? uint16_t(buttons | b) : 0xFFFF;  // 0xFFFF: a name that isn't a button
            });
        const std::string when(at == std::string_view::npos ? std::string_view{} : part.substr(at + 1));
        // Each number must be there (strtod consuming something) and finite.
        bool numbers = !when.empty();
        char* end = nullptr;
        const double seconds = numbers ? std::strtod(when.c_str(), &end) : -1;
        numbers = numbers && end != when.c_str();
        double duration = 0.1;
        if (numbers && *end == ':') {
            const char* d = end + 1;
            duration = std::strtod(d, &end);
            numbers = end != d;
        }
        if (at == std::string_view::npos || buttons == 0 || buttons == 0xFFFF || !numbers || *end || !std::isfinite(seconds) ||
            !std::isfinite(duration) || seconds < 0 || duration <= 0) {
            ok = false;
            bad = part;
            return;
        }
        out.push_back({seconds, seconds + duration, buttons});
    });
    return ok;
}

bool input_source() {
    load();
    return g_script.present || host::windowed();
}

uint16_t input_buttons(unsigned channel) {
    load();
    if (channel != 0) return 0;
    uint16_t held = host::windowed() ? host::keyboard_buttons() : 0;
    const double now = double(time_base() - g_start) / double(kTimeBaseHz);
    for (const ScriptedPress& p : g_script.presses)
        if (now >= p.from && now < p.to) held |= p.buttons;
    return held;
}

}  // namespace qilin::rt
