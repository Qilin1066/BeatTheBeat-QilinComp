// revolution/vi (video interface) and revolution/sc (system configuration),
// native: what lifted code calls of them, added as bring-up reaches each.
// Presentation itself comes with GX on Vulkan. Runtime code: no std::format.
#include "console.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

// VI's TV formats (VIGetTvFormat).
constexpr uint32_t kTvPal = 1, kTvEuRgb60 = 5;

struct Vi {
    bool initialised = false;
} vi;

// ---- SC: the console's settings. Each returns a byte: 1 on, 0 off. ----
QILIN_NATIVE(SCGetProgressiveMode) { c.r[3] = console().progressive ? 1 : 0; }
QILIN_NATIVE(SCGetEuRgb60Mode) { c.r[3] = console().eurgb60 ? 1 : 0; }
QILIN_NATIVE(SCGetAspectRatio) { c.r[3] = console().widescreen ? 1 : 0; }  // 0 is 4:3
QILIN_NATIVE(SCGetLanguage) { c.r[3] = console().language; }

// ---- VI ----
QILIN_NATIVE(VIInit) { vi.initialised = true; }
// The format the console is showing. A PAL console set to 60 Hz shows
// EuRGB60. The game only asks whether it is NTSC (0) here.
QILIN_NATIVE(VIGetTvFormat) { c.r[3] = console().eurgb60 ? kTvEuRgb60 : kTvPal; }
// 1: the cable can carry progressive scan.
QILIN_NATIVE(VIGetDTVStatus) { c.r[3] = console().component ? 1 : 0; }

}  // namespace
