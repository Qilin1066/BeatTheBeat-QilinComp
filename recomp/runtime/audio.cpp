// Audio: revolution/ai (the audio DMA) and revolution/dsp (the DSP), native
// (docs/runtime.md). AX, the SDK's voice library above them, is lifted whole:
// it is CPU code that keeps the voices, syncs their parameter blocks (PBs)
// and builds each frame's command list for the DSP. Here the DSP carries out
// that command list instead of the DSP's microcode, and AI's DMA interrupt is
// the audio clock: every 96 stereo samples at 32 kHz (3 ms), as on the Wii.
//
// The protocol is AX's own (its __AXOutInitDSP, __AXOutNewFrame and DSP
// callbacks): AX registers a DSP task whose init callback it waits for; each
// frame it mails 0xBABE0080 then the command list's address; the DSP's
// frame-done ("resume") callback lets AX start the next one.
//
// For now the DSP decodes the command list's set-up and the voice chain, and
// outputs silence while no voice plays: what the console outputs then. A
// playing voice stops the game with a message until the mixer exists.
// Runtime code: no std::format.
#include <cstdio>

#include "interrupts.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

constexpr uint32_t kSampleRate = 32000, kFrameSamples = 96;
constexpr uint32_t kFrameBytes = kFrameSamples * 4;  // stereo, 16-bit

// ---- AI ----
struct Ai {
    bool initialised = false, running = false;
    uint32_t callback = 0;          // AIRegisterDMACallback
    uint32_t start = 0, length = 0;  // AIInitDMA: the buffer the next DMA plays
} ai;

void dma_interrupt(Context& c) {
    if (!ai.running) return;
    ++stats().audio_frames;
    if (ai.callback) call_guest(c, ai.callback);
}

QILIN_NATIVE(AIInit) { ai.initialised = true; }
QILIN_NATIVE(AICheckInit) { c.r[3] = ai.initialised ? 1 : 0; }
QILIN_NATIVE(AIRegisterDMACallback) {
    const uint32_t old = ai.callback;
    ai.callback = c.r[3];
    c.r[3] = old;
}
QILIN_NATIVE(AIInitDMA) {
    ai.start = c.r[3], ai.length = c.r[4];
    if (ai.length != kFrameBytes) {
        std::fprintf(stderr, "qilin_run: AI: a DMA of %u bytes (AX's frames are %u)\n", ai.length, kFrameBytes);
        throw Stop{Stop::Unsupported, c.lr - 4, ai.length};
    }
}
QILIN_NATIVE(AIStartDMA) {
    if (ai.running) return;
    ai.running = true;
    add_periodic(kTimeBaseHz * kFrameSamples / kSampleRate, dma_interrupt);
}
// The DMA interrupt comes when a buffer has played out: AX asks at that
// moment, so nothing is left.
QILIN_NATIVE(AIGetDMABytesLeft) { c.r[3] = 0; }
QILIN_NATIVE(AIGetDMAStartAddr) { c.r[3] = ai.start; }
QILIN_NATIVE(AIGetDMALength) { c.r[3] = ai.length; }

// ---- the DSP ----
// DSPTaskInfo, as AX fills it: the callbacks at +40 (init), +44 (resume),
// +48 (done), +52 (request); each gets the task.
constexpr uint32_t kTaskInit = 40, kTaskResume = 44;
constexpr uint32_t kMailCommandList = 0xBABE0080u;
// AXPB, as AX's setters write it: the next PB's address (two halves) at +0,
// the state at +16 (1 running).
constexpr uint32_t kPbState = 16, kPbRunning = 1;

struct Dsp {
    bool initialised = false;
    uint32_t task = 0;
    bool expect_list = false;  // the command list's address comes after kMailCommandList
} dsp;

uint32_t half_pair(uint32_t a) { return uint32_t(peek16(a)) << 16 | peek16(a + 2); }

// One frame: the command list's set-up (command 0: the studio; 1, 2 or 3 by
// output mode: the mix buffers; 4: the first PB), then the voices.
void run_frame(uint32_t list) {
    const uint16_t c0 = peek16(list), c1 = peek16(list + 6), c2 = peek16(list + 12);
    if (c0 != 0 || c1 < 1 || c1 > 3 || c2 != 4) {
        std::fprintf(stderr, "qilin_run: DSP: command list %08X starts %04X %04X %04X, not AX's set-up\n", list, c0, c1,
                     c2);
        throw Stop{Stop::Unsupported, 0, list};
    }
    uint32_t pb = half_pair(list + 14) | 0x80000000u;
    for (unsigned n = 0; pb != 0x80000000u; ++n) {
        if (n > 256) {
            std::fprintf(stderr, "qilin_run: DSP: the voice chain from %08X doesn't end\n", half_pair(list + 14));
            throw Stop{Stop::Unsupported, 0, pb};
        }
        if (peek16(pb + kPbState) == kPbRunning) {
            std::fprintf(stderr, "qilin_run: a voice plays (PB %08X): the mixer isn't written yet\n", pb);
            throw Stop{Stop::Unsupported, 0, pb};
        }
        pb = half_pair(pb) | 0x80000000u;
    }
    // No voice plays: the output is silence, which the buffers already hold.
}

QILIN_NATIVE(DSPInit) { dsp.initialised = true; }
QILIN_NATIVE(DSPCheckInit) { c.r[3] = dsp.initialised ? 1 : 0; }
// DSPAddTask(task): the DSP boots the task, which calls its init callback.
// AX spins on what that callback sets (a loop with no call in it), so it
// runs before this returns, as the console's DSP does while the CPU waits.
QILIN_NATIVE(DSPAddTask) {
    if (dsp.task) throw Stop{Stop::Unsupported, c.lr - 4, c.r[3]};  // only AX's task is expected
    dsp.task = c.r[3];
    const uint32_t init = peek32(dsp.task + kTaskInit);
    if (init) {
        c.r[3] = dsp.task;
        call_guest(c, init);
    }
    c.r[3] = dsp.task;
}
// The mailbox to the DSP takes mail at once.
QILIN_NATIVE(DSPCheckMailToDSP) { c.r[3] = 0; }
QILIN_NATIVE(DSPSendMailToDSP) {
    const uint32_t mail = c.r[3];
    if (!dsp.expect_list) {
        if (mail != kMailCommandList) {
            std::fprintf(stderr, "qilin_run: DSP: mail %08X (only AX's command lists are handled)\n", mail);
            throw Stop{Stop::Unsupported, c.lr - 4, mail};
        }
        dsp.expect_list = true;
        return;
    }
    dsp.expect_list = false;
    run_frame(mail);
    // The frame is done: the DSP yields, and AX's resume callback runs.
    const uint32_t task = dsp.task, resume = peek32(task + kTaskResume);
    post_interrupt([task, resume](Context& ctx) {
        ctx.r[3] = task;
        call_guest(ctx, resume);
    });
}
// AX asks the DSP to yield when a DMA interrupt finds it busy. Frames finish
// at once here, so there is nothing to hurry.
QILIN_NATIVE(DSPAssertTask) { c.r[3] = dsp.task; }

}  // namespace
