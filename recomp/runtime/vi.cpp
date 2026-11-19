// revolution/vi (video interface) and revolution/sc (system configuration),
// native: what lifted code calls of them, added as bring-up reaches each.
// Presentation itself comes with GX on Vulkan. Runtime code: no std::format.
#include "console.hpp"
#include "interrupts.hpp"
#include "machine.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

// VI's TV formats (VIGetTvFormat).
constexpr uint32_t kTvPal = 1, kTvEuRgb60 = 5;

struct Vi {
    bool initialised = false;
    // VIConfigure's render mode (GXRenderModeObj): TV mode +0, frame buffer
    // width +4, EFB height +6, XFB height +8, origin and size +10..+16, XFB
    // mode +20, field rendering +24.
    uint32_t tv_mode = 0, xfb_mode = 0;
    uint16_t fb_width = 0, efb_height = 0, xfb_height = 0, vi_width = 0, vi_height = 0;
    uint32_t next_fb = 0, pending_fb = 0, shown_fb = 0;
    bool black = true, flushed = false;
    uint32_t pre_cb = 0, post_cb = 0;
    uint32_t retraces = 0;
    uint32_t queue = 0;  // an OSThreadQueue in guest memory: VIWaitForRetrace sleeps on it
} vi;

// The field rate: NTSC timing at 60 Hz (EuRGB60 included), PAL at 50 Hz.
uint64_t retrace_period() {
    return console().eurgb60 ? kTimeBaseHz * 1001 / 60000 : kTimeBaseHz / 50;
}

// The retrace, as the SDK's VI interrupt runs it: the pre-retrace callback,
// the frame buffer VIFlush asked for goes on screen, the post-retrace
// callback, and the threads in VIWaitForRetrace wake.
void retrace(Context& c) {
    ++vi.retraces;
    ++stats().retraces;
    static uint32_t wakeup = 0;
    if (!wakeup) wakeup = require_function("OSWakeupThread");
    if (vi.pre_cb) {
        c.r[3] = vi.retraces;
        call_guest(c, vi.pre_cb);
    }
    if (vi.flushed) vi.shown_fb = vi.pending_fb, vi.flushed = false;
    if (vi.post_cb) {
        c.r[3] = vi.retraces;
        call_guest(c, vi.post_cb);
    }
    c.r[3] = vi.queue;
    call_guest(c, wakeup);
}

// ---- SC: the console's settings. Each returns a byte: 1 on, 0 off. ----
QILIN_NATIVE(SCGetProgressiveMode) { c.r[3] = console().progressive ? 1 : 0; }
QILIN_NATIVE(SCGetEuRgb60Mode) { c.r[3] = console().eurgb60 ? 1 : 0; }
QILIN_NATIVE(SCGetAspectRatio) { c.r[3] = console().widescreen ? 1 : 0; }  // 0 is 4:3
QILIN_NATIVE(SCGetLanguage) { c.r[3] = console().language; }
QILIN_NATIVE(SCGetSoundMode) { c.r[3] = console().sound_mode; }
// The settings are the runtime's, so there is nothing to load: ready at once
// (SCCheckStatus: 0 ready, 1 busy, as OSInit's wait loop reads it).
QILIN_NATIVE(SCInit) {}
QILIN_NATIVE(SCCheckStatus) { c.r[3] = 0; }

// ---- VI ----
QILIN_NATIVE(VIInit) {
    if (vi.initialised) return;
    vi.initialised = true;
    vi.queue = guest_alloc(8, 4);  // an empty OSThreadQueue: head and tail 0
    add_periodic(retrace_period(), retrace);
}
QILIN_NATIVE(VIConfigure) {
    const uint32_t m = c.r[3];
    vi.tv_mode = peek32(m), vi.fb_width = peek16(m + 4), vi.efb_height = peek16(m + 6), vi.xfb_height = peek16(m + 8);
    vi.vi_width = peek16(m + 14), vi.vi_height = peek16(m + 16), vi.xfb_mode = peek32(m + 20);
}
QILIN_NATIVE(VISetNextFrameBuffer) { vi.next_fb = c.r[3]; }
QILIN_NATIVE(VIFlush) {
    vi.pending_fb = vi.next_fb;
    vi.flushed = true;
}
QILIN_NATIVE(VISetBlack) { vi.black = c.r[3] & 0xFF; }
QILIN_NATIVE(VISetPreRetraceCallback) {
    const uint32_t old = vi.pre_cb;
    vi.pre_cb = c.r[3];
    c.r[3] = old;
}
QILIN_NATIVE(VISetPostRetraceCallback) {
    const uint32_t old = vi.post_cb;
    vi.post_cb = c.r[3];
    c.r[3] = old;
}
// Sleeps until the next retrace, as the SDK's: interrupts off, then the
// thread sleeps on VI's queue until the retrace count moves (other threads
// run meanwhile).
QILIN_NATIVE(VIWaitForRetrace) {
    static uint32_t sleep = 0;
    if (!sleep) sleep = require_function("OSSleepThread");
    const bool level = interrupts_enabled();
    set_interrupts_enabled(false);
    for (const uint32_t count = vi.retraces; count == vi.retraces;) {
        c.r[3] = vi.queue;
        call_guest(c, sleep);
    }
    // OSRestoreInterrupts turning them back on is a safe point: what came
    // due meanwhile runs now, not at the next one.
    set_interrupts_enabled(level);
    if (level) deliver_due(c);
}
// The format the console is showing. A PAL console set to 60 Hz shows
// EuRGB60. The game only asks whether it is NTSC (0) here.
QILIN_NATIVE(VIGetTvFormat) { c.r[3] = console().eurgb60 ? kTvEuRgb60 : kTvPal; }
// 1: the cable can carry progressive scan.
QILIN_NATIVE(VIGetDTVStatus) { c.r[3] = console().component ? 1 : 0; }

}  // namespace
