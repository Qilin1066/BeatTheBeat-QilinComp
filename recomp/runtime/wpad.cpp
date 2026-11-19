// revolution/wpad (the Wii Remote), native at its API. KPAD, the remote
// processing above it, is lifted.
//
// Until the runtime has a host input backend, no remote is connected: every
// channel reports WPAD_ERR_NO_CONTROLLER. That is a state a console is in
// whenever remotes are off or out of batteries, so KPAD and the Home Button
// Menu handle it as on hardware; nothing is made up. Functions are added as
// bring-up reaches them, each checked against how its callers use the
// result. Runtime code: no std::format.
#include "console.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

constexpr unsigned kChannels = 4;
constexpr uint32_t kStateSetup = 3;  // WPADGetStatus: ready (KPADInitEx tests for it)

struct Wpad {
    uint32_t alloc = 0, free = 0;  // WPADRegisterAllocator: WPAD's memory, which the native WPAD doesn't need
    bool initialised = false, by_kpad = false, motor_enabled = true;
    uint32_t connect_cb[kChannels] = {}, sampling_cb[kChannels] = {}, extension_cb[kChannels] = {};
} wpad;

uint32_t channel(const Context& c) {
    if (c.r[3] >= kChannels) throw Stop{Stop::Unsupported, c.lr - 4, c.r[3]};
    return c.r[3];
}
// Sets a per-channel callback and returns the old one, as the SDK's setters do.
void swap_callback(Context& c, uint32_t (&slot)[kChannels]) {
    const uint32_t ch = channel(c), old = slot[ch];
    slot[ch] = c.r[4];
    c.r[3] = old;
}

QILIN_NATIVE(WPADRegisterAllocator) { wpad.alloc = c.r[3], wpad.free = c.r[4]; }
QILIN_NATIVE(WPADInit) { wpad.initialised = true; }
QILIN_NATIVE(WPADGetStatus) { c.r[3] = wpad.initialised ? kStateSetup : 0; }
// The console's settings for the pointer: sensitivity (1-5) and whether the
// sensor bar is above the TV.
QILIN_NATIVE(WPADGetDpdSensitivity) { c.r[3] = console().dpd_sensitivity; }
QILIN_NATIVE(WPADGetSensorBarPosition) { c.r[3] = console().sensor_bar_above ? 1 : 0; }
QILIN_NATIVE(WPADSetConnectCallback) { swap_callback(c, wpad.connect_cb); }
QILIN_NATIVE(WPADSetSamplingCallback) { swap_callback(c, wpad.sampling_cb); }
QILIN_NATIVE(WPADSetExtensionCallback) { swap_callback(c, wpad.extension_cb); }
QILIN_NATIVE(WPADSetCallbackByKPAD) { wpad.by_kpad = c.r[3] & 0xFF; }
QILIN_NATIVE(WPADIsUsedCallbackByKPAD) { c.r[3] = wpad.by_kpad ? 1 : 0; }
// MotionPlus: none, with no remote.
QILIN_NATIVE(WPADiGetMplsStatus) {
    channel(c);
    c.r[3] = 0;
}
// Rumble: nothing to rumble with no remote connected.
QILIN_NATIVE(WPADControlMotor) { channel(c); }
QILIN_NATIVE(WPADEnableMotor) { wpad.motor_enabled = c.r[3] & 0xFF; }
QILIN_NATIVE(WPADIsMotorEnabled) { c.r[3] = wpad.motor_enabled ? 1 : 0; }

// ---- revolution/pad: GameCube controllers. None are plugged in. ----
QILIN_NATIVE(PADInit) { c.r[3] = 1; }

}  // namespace
