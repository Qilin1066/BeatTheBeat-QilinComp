// The library plan for the supported build: which code is lifted, which is
// replaced by native code at its API, and which is dropped. The ranges are the
// table in docs/code-analysis.md (keep the two in step; the tests check this
// one for order and gaps).
//
// `qilin lift --runtime` uses it: a function on the replaced or dropped side
// becomes a call into the runtime's registry of native functions instead of
// lifted code, unless it is named in lift_anyway().
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace qilin {

enum class Plan { Lift, Replace, Drop };

struct Library {
    uint32_t lo, hi;        // PAL addresses, [lo, hi)
    std::string_view name;  // short: "os", "gx", "nw4r/snd"
    Plan plan;
};

std::span<const Library> libraries();
// The library holding a function that starts at `addr`; nullptr if none does.
const Library* library_at(uint32_t addr);

// Functions inside replaced libraries that are lifted all the same: software
// with no hardware under it, which the runtime calls or lifted code needs
// exactly as the SDK wrote it. By name (the decomp's), so no PAL address list
// is kept here.
bool lift_anyway(std::string_view name);
std::span<const std::string_view> lift_anyway_names();

// The PAL symbol list tools/analysis/port_symbols.py writes
// ("name = .text:0x80038314; // ..."): address -> name. Throws
// std::runtime_error with the reason if the file can't be read or holds no
// symbols.
std::map<uint32_t, std::string> load_symbols(const std::string& path);

}  // namespace qilin
