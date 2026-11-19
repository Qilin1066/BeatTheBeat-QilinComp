// revolution/wpad (the Wii Remote), native at its API. KPAD, the remote
// processing above it, is lifted.
//
// The remote: WPAD drives KPAD through two callbacks, as KPAD's code shows:
// the connect callback (chan, 0 connected / else disconnected) and, once
// connected, the sampling callback for every report, in which KPAD reads
// the latest status with WPADRead. A remote is connected on channel 0 only
// when the runtime has an input source: for now QILIN_INPUT, a script of
// button presses (input.hpp). Without one, every channel reports
// WPAD_ERR_NO_CONTROLLER, a state a console is in whenever remotes are off.
//
// Nothing is made up for motion or the pointer: the pointer sees no IR
// objects (the remote isn't aimed at the sensor bar), acceleration and its
// calibration read zero (KPAD then uses its own default). The game is
// played with buttons. Functions are added as bring-up reaches them, each
// checked against how its callers use the result. Runtime code: no
// std::format.
#include <cstdio>

#include "console.hpp"
#include "input.hpp"
#include "interrupts.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

constexpr unsigned kChannels = 4;
constexpr uint32_t kStateSetup = 3;  // WPADGetStatus: ready (KPADInitEx tests for it)
// How often a remote reports. TO CONFIRM against the console: the game
// timestamps samples, so this matters for rhythm timing.
constexpr uint64_t kReportPeriod = kTimeBaseHz / 200;
// A remote takes a moment to connect after WPAD starts.
constexpr uint64_t kConnectDelay = kTimeBaseHz / 10;
// WPADStatus, as KPAD reads it: buttons +0, acceleration +2/+4/+6, four IR
// objects from +8 (8 bytes each), device type +40, error +41.
constexpr uint32_t kStatusBytes = 42, kStatusDev = 40, kStatusErr = 41;
constexpr uint8_t kDevCore = 0;

struct Wpad {
    uint32_t alloc = 0, free = 0;  // WPADRegisterAllocator: WPAD's memory, which the native WPAD doesn't need
    bool initialised = false, by_kpad = false, motor_enabled = true;
    uint32_t connect_cb[kChannels] = {}, sampling_cb[kChannels] = {}, extension_cb[kChannels] = {};
    bool connected[kChannels] = {};
    uint32_t format[kChannels] = {};
    bool dpd[kChannels] = {};
} wpad;

void call_back(Context& c, uint32_t fn, uint32_t r3, uint32_t r4) {
    c.r[3] = r3, c.r[4] = r4;
    call_guest(c, fn);
}

// The remote's reports, as WPAD's interrupt delivers them: the sampling
// callback, which reads the status with WPADRead.
void report(Context& c) {
    if (wpad.connected[0] && wpad.sampling_cb[0]) {
        ++stats().remote_reports;
        call_back(c, wpad.sampling_cb[0], 0, 0);
    }
}

const char* name_of(uint32_t fn) {
    const Symbol* s = fn ? symbol_at(fn) : nullptr;
    return s ? s->name : "none";
}

// Connecting channel 0: the connect callback with WPAD_ERR_NONE, then
// reports at the remote's rate.
void connect(Context& c) {
    wpad.connected[0] = true;
    ++stats().remotes_connected;
    const uint32_t cb = wpad.connect_cb[0];
    if (cb) call_back(c, cb, 0, 0);
    std::fprintf(stderr, "qilin_run: Wii Remote connected (connect callback %s, sampling callback %s)\n", name_of(cb),
                 name_of(wpad.sampling_cb[0]));
    add_periodic(kReportPeriod, report);
}

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
QILIN_NATIVE(WPADInit) {
    if (wpad.initialised) return;
    wpad.initialised = true;
    if (input_source()) post_interrupt(connect, kConnectDelay);
}
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
constexpr int32_t kErrNoController = -1;

// WPADProbe(chan, u32* type): whether a remote is there and what kind (a
// core remote, no extension); WPAD_ERR_NO_CONTROLLER if none is.
QILIN_NATIVE(WPADProbe) {
    const uint32_t ch = channel(c);
    if (!wpad.connected[ch]) {
        c.r[3] = static_cast<uint32_t>(kErrNoController);
        return;
    }
    if (c.r[4]) poke32(c.r[4], kDevCore);
    c.r[3] = 0;
}

// WPADRead(chan, status): the latest report. Buttons from the input source;
// no motion and no IR objects (see the top of this file).
QILIN_NATIVE(WPADRead) {
    const uint32_t ch = channel(c), st = c.r[4];
    for (uint32_t i = 0; i < kStatusBytes; ++i) poke8(st + i, 0);
    if (!wpad.connected[ch]) {
        poke8(st + kStatusErr, static_cast<uint8_t>(kErrNoController));
        return;
    }
    poke16(st, input_buttons(ch));
    poke8(st + kStatusDev, kDevCore);
}

QILIN_NATIVE(WPADGetDataFormat) { c.r[3] = wpad.format[channel(c)]; }
// WPADSetDataFormat(chan, format): which report the remote sends. The
// virtual remote's report is the same for every format.
QILIN_NATIVE(WPADSetDataFormat) {
    const uint32_t ch = channel(c);
    if (!wpad.connected[ch]) {
        c.r[3] = static_cast<uint32_t>(kErrNoController);
        return;
    }
    wpad.format[ch] = c.r[4];
    c.r[3] = 0;
}
// WPADGetAccGravityUnit(chan, type, s16 acc[3]): the accelerometer's 1 g per
// axis, from the remote's calibration. None here: zero, and KPAD uses its
// own default.
QILIN_NATIVE(WPADGetAccGravityUnit) {
    channel(c);
    for (uint32_t i = 0; i < 3; ++i) poke16(c.r[5] + 2 * i, 0);
}
// ---- the remote's speaker ----
// The control protocol is modelled: commands succeed (the callback gets
// (chan, 0) when the remote has done it), the speaker is on between "on"
// and "off", and stream data is accepted while it's on. The sound itself
// isn't played yet: the host has no remote speaker (a gap, until host audio
// exists).
constexpr uint32_t kSpeakerOff = 0, kSpeakerOn = 1;
bool g_speaker_on[kChannels] = {};
uint8_t g_speaker_volume[kChannels] = {};

QILIN_NATIVE(WPADControlSpeaker) {
    const uint32_t ch = channel(c), cmd = c.r[4], cb = c.r[5];
    if (!wpad.connected[ch]) {
        c.r[3] = static_cast<uint32_t>(kErrNoController);
        return;
    }
    if (cmd == kSpeakerOn) g_speaker_on[ch] = true;
    if (cmd == kSpeakerOff) g_speaker_on[ch] = false;
    if (cb) post_interrupt([ch, cb](Context& ctx) { call_back(ctx, cb, ch, 0); }, kIoLatency);
    c.r[3] = 0;
}
QILIN_NATIVE(WPADIsSpeakerEnabled) { c.r[3] = g_speaker_on[channel(c)] ? 1 : 0; }
QILIN_NATIVE(WPADCanSendStreamData) { c.r[3] = g_speaker_on[channel(c)] ? 1 : 0; }
// WPADSendStreamData(chan, data, length): accepted, not played (see above).
QILIN_NATIVE(WPADSendStreamData) {
    const uint32_t ch = channel(c);
    c.r[3] = g_speaker_on[ch] ? 0 : static_cast<uint32_t>(kErrNoController);
}
QILIN_NATIVE(WPADGetSpeakerVolume) { c.r[3] = g_speaker_volume[0]; }
QILIN_NATIVE(WPADSetSpeakerVolume) { g_speaker_volume[0] = static_cast<uint8_t>(c.r[3]); }

QILIN_NATIVE(WPADIsDpdEnabled) { c.r[3] = wpad.dpd[channel(c)] ? 1 : 0; }
// WPADControlDpd(chan, command, callback): the pointer camera on (non-zero)
// or off; the callback gets (chan, result) when the remote has done it.
QILIN_NATIVE(WPADControlDpd) {
    const uint32_t ch = channel(c), cb = c.r[5];
    if (!wpad.connected[ch]) {
        c.r[3] = static_cast<uint32_t>(kErrNoController);
        return;
    }
    wpad.dpd[ch] = c.r[4] != 0;
    if (cb)
        post_interrupt([ch, cb](Context& ctx) { call_back(ctx, cb, ch, 0); }, kIoLatency);
    c.r[3] = 0;
}

// No remote is connected, so there is none to disconnect.
QILIN_NATIVE(WPADDisconnect) { channel(c); }
// Rumble: nothing to rumble with no remote connected.
QILIN_NATIVE(WPADControlMotor) { channel(c); }
QILIN_NATIVE(WPADEnableMotor) { wpad.motor_enabled = c.r[3] & 0xFF; }
QILIN_NATIVE(WPADIsMotorEnabled) { c.r[3] = wpad.motor_enabled ? 1 : 0; }

// ---- revolution/pad: GameCube controllers. None are plugged in. ----
QILIN_NATIVE(PADInit) { c.r[3] = 1; }
// PADRead(PADStatus status[4]): 12 bytes each, the error byte at +10. Every
// port reports PAD_ERR_NO_CONTROLLER (-1), with no buttons or sticks; the
// game then calls PADReset for those ports, as games probe empty ports.
QILIN_NATIVE(PADRead) {
    for (uint32_t p = 0; p < 4; ++p) {
        const uint32_t st = c.r[3] + 12 * p;
        for (uint32_t i = 0; i < 12; ++i) poke8(st + i, 0);
        poke8(st + 10, 0xFF);  // -1
    }
    c.r[3] = 0;
}
QILIN_NATIVE(PADReset) { c.r[3] = 1; }  // nothing plugged in: nothing to reset

}  // namespace
