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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "guest.hpp"
#include "host.hpp"

namespace qilin::rt {

namespace {

struct Press {
    uint64_t from, to;  // time-base ticks
    uint16_t buttons;
};

struct Script {
    bool parsed = false, present = false;
    uint64_t start = 0;
    std::vector<Press> presses;
} g_script;

uint16_t button(const std::string& name) {
    struct Name {
        const char* name;
        uint16_t bit;
    };
    static const Name names[] = {{"A", kButtonA},         {"B", kButtonB},       {"1", kButton1},
                                 {"2", kButton2},         {"PLUS", kButtonPlus}, {"MINUS", kButtonMinus},
                                 {"HOME", kButtonHome},   {"UP", kButtonUp},     {"DOWN", kButtonDown},
                                 {"LEFT", kButtonLeft},   {"RIGHT", kButtonRight}};
    for (const Name& n : names)
        if (name == n.name) return n.bit;
    return 0;
}

[[noreturn]] void bad_script(const std::string& part) {
    std::fprintf(stderr, "qilin_run: QILIN_INPUT: can't read \"%s\" (BUTTON[+BUTTON]@SECONDS[:DURATION])\n",
                 part.c_str());
    std::fflush(stderr);
    std::_Exit(2);
}

void parse() {
    if (g_script.parsed) return;
    g_script.parsed = true;
    const char* v = std::getenv("QILIN_INPUT");
    if (!v || !*v) return;
    g_script.present = true;
    g_script.start = time_base();
    std::string all = v;
    size_t i = 0;
    while (i <= all.size()) {
        const size_t comma = all.find(',', i);
        const std::string part = all.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        i = comma == std::string::npos ? all.size() + 1 : comma + 1;
        if (part.empty()) continue;
        const size_t at = part.find('@');
        if (at == std::string::npos) bad_script(part);
        uint16_t buttons = 0;
        const std::string names = part.substr(0, at);
        for (size_t j = 0; j <= names.size();) {
            const size_t plus = names.find('+', j);
            const uint16_t b = button(names.substr(j, plus == std::string::npos ? std::string::npos : plus - j));
            if (!b) bad_script(part);
            buttons |= b;
            j = plus == std::string::npos ? names.size() + 1 : plus + 1;
        }
        const std::string when = part.substr(at + 1);
        char* end = nullptr;
        const double seconds = std::strtod(when.c_str(), &end);
        double duration = 0.1;
        if (*end == ':') duration = std::strtod(end + 1, &end);
        if (*end || seconds < 0 || duration <= 0) bad_script(part);
        const auto ticks = [](double s) { return static_cast<uint64_t>(s * static_cast<double>(kTimeBaseHz)); };
        g_script.presses.push_back({g_script.start + ticks(seconds), g_script.start + ticks(seconds + duration), buttons});
    }
}

}  // namespace

bool input_source() {
    parse();
    return g_script.present || host::windowed();
}

uint16_t input_buttons(unsigned channel) {
    parse();
    if (channel != 0) return 0;
    const uint64_t now = time_base();
    uint16_t held = host::windowed() ? host::keyboard_buttons() : 0;
    for (const Press& p : g_script.presses)
        if (now >= p.from && now < p.to) held |= p.buttons;
    return held;
}

}  // namespace qilin::rt
