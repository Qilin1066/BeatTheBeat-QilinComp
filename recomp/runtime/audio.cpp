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
// The mixer (run_frame) carries out the command list as AX builds it
// (__AXNextFrame) and as AX and NW4R use the voices' parameter blocks (PBs).
// What game logic depends on is exact: play positions (16.16 resampling
// steps), loops and ends, DSP-ADPCM decoding with its history and loop
// context, the volume envelope and mix ramps (per sample, 96 a frame, as
// NW4R computes them), and the write-back AX reads. Where the DSP's own
// arithmetic has no source, the output is an approximation, each marked
// APPROX: the polyphase resampling filter (a 4-tap cubic here), rounding,
// and the compressor (saturation here). Features with no source yet stop
// when used (biquad, interaural delay, surround output, Dolby Pro Logic II).
// docs/runtime.md has the layouts. Runtime code: no std::format.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "host.hpp"
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

// QILIN_AUDIO_DUMP=<file.wav>: what AI plays, as a WAV file (16-bit stereo,
// 32 kHz), to check the mixer by ear until there is host audio output.
struct Dump {
    std::FILE* f = nullptr;
    uint32_t bytes = 0;
    bool checked = false;
} g_dump;

void put32le(std::FILE* f, uint32_t v) {
    const unsigned char b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void write_wav_sizes() {
    std::fseek(g_dump.f, 4, SEEK_SET);
    put32le(g_dump.f, 36 + g_dump.bytes);
    std::fseek(g_dump.f, 40, SEEK_SET);
    put32le(g_dump.f, g_dump.bytes);
    std::fseek(g_dump.f, 0, SEEK_END);
}
void dump_frame(uint32_t start) {
    if (!g_dump.checked) {
        g_dump.checked = true;
        const char* path = std::getenv("QILIN_AUDIO_DUMP");
        if (!path || !*path) return;
        g_dump.f = std::fopen(path, "wb");
        if (!g_dump.f) {
            std::fprintf(stderr, "qilin_run: QILIN_AUDIO_DUMP: can't write %s\n", path);
            std::_Exit(2);
        }
        static const unsigned char header[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                               16,  0,   0,   0,   1, 0, 2, 0, 0, 0x7D, 0, 0, 0, 0xF4, 1, 0,
                                               4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0};
        std::fwrite(header, 1, sizeof header, g_dump.f);  // 32000 Hz, 128000 bytes/s, 4-byte frames
    }
    if (!g_dump.f) return;
    unsigned char le[kFrameBytes];
    for (uint32_t i = 0; i < kFrameBytes; i += 2) {
        const uint16_t s = peek16(start + i);
        le[i] = uint8_t(s), le[i + 1] = uint8_t(s >> 8);
    }
    std::fwrite(le, 1, kFrameBytes, g_dump.f);
    g_dump.bytes += kFrameBytes;
    write_wav_sizes();
}

// What AI plays goes to the host's audio output (host byte order).
void play_frame(uint32_t start) {
    if (!host::windowed()) return;
    int16_t lr[kFrameSamples * 2];
    for (uint32_t i = 0; i < kFrameSamples * 2; ++i) lr[i] = static_cast<int16_t>(peek16(start + 2 * i));
    host::play_audio(lr, kFrameSamples);
}

void dma_interrupt(Context& c) {
    if (!ai.running) return;
    ++stats().audio_frames;
    dump_frame(ai.start);
    play_frame(ai.start);
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
struct Dsp {
    bool initialised = false;
    uint32_t task = 0;
    bool expect_list = false;  // the command list's address comes after kMailCommandList
} dsp;

uint32_t half_pair(uint32_t a) { return uint32_t(peek16(a)) << 16 | peek16(a + 2); }
void put_half_pair(uint32_t a, uint32_t v) {
    poke16(a, uint16_t(v >> 16));
    poke16(a + 2, uint16_t(v));
}
int16_t clamp16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -32768, 32767)); }

[[noreturn]] void unsupported(const char* what, uint32_t where) {
    std::fprintf(stderr, "qilin_run: DSP: %s (at %08X): not supported yet\n", what, where);
    throw Stop{Stop::Unsupported, 0, where};
}

// The DSP's copy of a voice (AXPB, 320 bytes), as AX's __AXServiceVPB and
// NW4R's setters lay it out.
namespace pb {
constexpr uint32_t kNext = 0, kSrcSelect = 8, kMixerCtrl = 12, kState = 16, kMix = 20, kItd = 68, kVeVolume = 106,
                   kVeDelta = 108, kLoopFlag = 110, kFormat = 112, kLoopAddr = 114, kEndAddr = 118, kCurAddr = 122,
                   kAdpcmCoefs = 126, kPredScale = 160, kYn1 = 162, kYn2 = 164, kRatio = 166, kFrac = 170,
                   kHistory = 172, kLoopPredScale = 180, kLoopYn1 = 182, kLoopYn2 = 184, kLpf = 186, kLpfYn1 = 188,
                   kLpfB0 = 190, kLpfA1 = 192, kBiquad = 194;
constexpr uint16_t kRunning = 1, kStopped = 0;
// Sample formats (the DSP's codes) and resampler types.
constexpr uint16_t kAdpcm = 0x00, kPcm16 = 0x0A, kPcm8 = 0x19;
constexpr uint16_t kSrcLinear = 1, kSrcNone = 2;  // 0: polyphase
}  // namespace pb

// Buses: main, aux A, B, C; channels L, R, S (surround).
constexpr unsigned kBuses = 4, kChannelsPerBus = 3;
using Bus = int32_t[kChannelsPerBus][kFrameSamples];
// The mix: 12 (volume, change per sample) pairs, and which bits of the
// mixer control enable each and its ramp (read from NW4R's SetVoiceMix).
struct MixSlot {
    unsigned bus, channel;
    uint32_t on, ramp;
};
constexpr MixSlot kMixSlots[12] = {
    {0, 0, 0x00000001, 0x00000004}, {0, 1, 0x00000002, 0x00000004}, {1, 0, 0x00010000, 0x00040000},
    {1, 1, 0x00020000, 0x00040000}, {2, 0, 0x00200000, 0x00800000}, {2, 1, 0x00400000, 0x00800000},
    {3, 0, 0x04000000, 0x10000000}, {3, 1, 0x08000000, 0x10000000}, {0, 2, 0x00000008, 0x00000010},
    {1, 2, 0x00080000, 0x00100000}, {2, 2, 0x01000000, 0x02000000}, {3, 2, 0x20000000, 0x40000000},
};

// Sample data: DSP addresses are physical (MEM1 from 0, MEM2 from
// 0x10000000), in nibbles for ADPCM, bytes for PCM8 and samples for PCM16.
uint32_t sample_memory(uint32_t phys, uint32_t pb_addr) {
    if (phys < 0x01800000u || (phys >= 0x10000000u && phys < 0x14000000u)) return phys | 0x80000000u;
    unsupported("sample data outside MEM1 and MEM2", pb_addr);
}

// A voice's sample source: decodes one sample at a time, following loops.
struct Source {
    uint32_t pb_addr;
    uint16_t format;
    uint32_t cur, end, loop;
    bool looping, ended = false;
    int16_t coefs[16];
    uint16_t ps;
    int16_t yn1, yn2;

    explicit Source(uint32_t p) : pb_addr(p) {
        format = peek16(p + pb::kFormat);
        if (format != pb::kAdpcm && format != pb::kPcm16 && format != pb::kPcm8) unsupported("a sample format", p);
        cur = half_pair(p + pb::kCurAddr), end = half_pair(p + pb::kEndAddr), loop = half_pair(p + pb::kLoopAddr);
        looping = peek16(p + pb::kLoopFlag) != 0;
        for (unsigned i = 0; i < 16; ++i) coefs[i] = int16_t(peek16(p + pb::kAdpcmCoefs + 2 * i));
        ps = peek16(p + pb::kPredScale);
        yn1 = int16_t(peek16(p + pb::kYn1)), yn2 = int16_t(peek16(p + pb::kYn2));
    }
    int16_t next() {
        if (ended) return 0;
        int16_t s = 0;
        switch (format) {
        case pb::kAdpcm: {
            // DSP-ADPCM: 8-byte frames, a header byte (predictor, scale) then
            // 14 four-bit samples. Nibble addresses; a frame's first two
            // nibbles are its header.
            if ((cur & 15) == 0) {
                ps = peek8(sample_memory(cur >> 1, pb_addr));
                cur += 2;
            }
            const uint8_t byte = peek8(sample_memory(cur >> 1, pb_addr));
            int nibble = (cur & 1) ? byte & 15 : byte >> 4;
            if (nibble >= 8) nibble -= 16;
            const unsigned pred = (ps >> 4) & 7;
            const int64_t v = (int64_t(nibble * (1 << (ps & 15))) << 11) + 1024 + int64_t(coefs[2 * pred]) * yn1 +
                              int64_t(coefs[2 * pred + 1]) * yn2;
            s = clamp16(v >> 11);
            yn2 = yn1, yn1 = s;
            break;
        }
        case pb::kPcm16: s = int16_t(peek16(sample_memory(cur * 2, pb_addr))); break;
        default: s = int16_t(int8_t(peek8(sample_memory(cur, pb_addr))) * 256); break;
        }
        if (cur == end) {
            if (!looping) {
                ended = true;
                return s;
            }
            cur = loop;
            if (format == pb::kAdpcm) {
                // The loop context: the predictor and history at the loop
                // start (streams rewrite it as they refill).
                ps = peek16(pb_addr + pb::kLoopPredScale);
                yn1 = int16_t(peek16(pb_addr + pb::kLoopYn1)), yn2 = int16_t(peek16(pb_addr + pb::kLoopYn2));
            }
        } else {
            ++cur;
        }
        return s;
    }
    void write_back() const {
        put_half_pair(pb_addr + pb::kCurAddr, cur);
        poke16(pb_addr + pb::kPredScale, ps);
        poke16(pb_addr + pb::kYn1, uint16_t(yn1));
        poke16(pb_addr + pb::kYn2, uint16_t(yn2));
    }
};

// APPROX: the DSP's polyphase filter (its coefficient tables are in the DSP's
// ROM) is a Catmull-Rom cubic here, between h[1] and h[2].
int16_t cubic(const int16_t h[4], uint32_t frac) {
    const float t = float(frac) / 65536.0f;
    const float a = -0.5f * h[0] + 1.5f * h[1] - 1.5f * h[2] + 0.5f * h[3];
    const float b = float(h[0]) - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
    const float cc = -0.5f * h[0] + 0.5f * h[2];
    return clamp16(int64_t(((a * t + b) * t + cc) * t + float(h[1])));
}

// One voice for one frame: decode, resample, filter, envelope, mix into the
// buses; then the DSP's state goes back into the PB for AX to read.
void mix_voice(uint32_t p, Bus* buses) {
    if (peek16(p + pb::kItd) != 0) unsupported("interaural time delay", p);
    if (peek16(p + pb::kBiquad) != 0) unsupported("the biquad filter", p);
    Source src(p);
    const uint16_t src_select = peek16(p + pb::kSrcSelect);
    if (src_select > pb::kSrcNone) unsupported("a resampler type", p);
    const uint32_t ratio = half_pair(p + pb::kRatio);
    uint32_t frac = peek16(p + pb::kFrac);
    int16_t h[4];
    for (unsigned i = 0; i < 4; ++i) h[i] = int16_t(peek16(p + pb::kHistory + 2 * i));
    const bool lpf = peek16(p + pb::kLpf) != 0;
    const int32_t lpf_b0 = peek16(p + pb::kLpfB0), lpf_a1 = peek16(p + pb::kLpfA1);
    int32_t lpf_y = int16_t(peek16(p + pb::kLpfYn1));
    int32_t ve = peek16(p + pb::kVeVolume);
    const int32_t ve_delta = int16_t(peek16(p + pb::kVeDelta));
    const uint32_t ctrl = half_pair(p + pb::kMixerCtrl);
    int32_t vol[12], delta[12];
    for (unsigned m = 0; m < 12; ++m) {
        vol[m] = peek16(p + pb::kMix + 4 * m);
        delta[m] = (ctrl & kMixSlots[m].ramp) ? int16_t(peek16(p + pb::kMix + 4 * m + 2)) : 0;
    }
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
        // The 16.16 step: each whole step takes the next source sample.
        frac += ratio;
        for (; frac >= 0x10000u; frac -= 0x10000u) {
            h[0] = h[1], h[1] = h[2], h[2] = h[3];
            h[3] = src.next();
        }
        int32_t s = src_select == pb::kSrcNone     ? h[3]
                    : src_select == pb::kSrcLinear ? h[1] + int32_t((int64_t(h[2] - h[1]) * frac) >> 16)
                                                   : cubic(h, frac);
        if (lpf) s = lpf_y = int32_t((int64_t(lpf_b0) * s + int64_t(lpf_a1) * lpf_y) >> 15);
        s = int32_t((int64_t(s) * ve) >> 15);
        ve = std::clamp(ve + ve_delta, 0, 0xFFFF);
        for (unsigned m = 0; m < 12; ++m) {
            const MixSlot& slot = kMixSlots[m];
            if (!(ctrl & slot.on)) continue;
            buses[slot.bus][slot.channel][i] += int32_t((int64_t(s) * vol[m]) >> 15);
            vol[m] = std::clamp(vol[m] + delta[m], 0, 0xFFFF);
        }
    }
    src.write_back();
    if (src.ended) poke16(p + pb::kState, pb::kStopped);
    poke16(p + pb::kFrac, uint16_t(frac));
    for (unsigned i = 0; i < 4; ++i) poke16(p + pb::kHistory + 2 * i, uint16_t(h[i]));
    poke16(p + pb::kLpfYn1, uint16_t(lpf_y));
    poke16(p + pb::kVeVolume, uint16_t(ve));
    for (unsigned m = 0; m < 12; ++m) poke16(p + pb::kMix + 4 * m, uint16_t(vol[m]));
    ++stats().voice_frames;
}

// An aux bus's buffers (32-bit samples, L, R and S one after the other):
// the DSP writes the bus to `in` for AX's aux callback and mixes `out`, which
// the callback processed in an earlier frame, back into the main bus.
void aux_round_trip(const int32_t (&bus)[kChannelsPerBus][kFrameSamples], uint32_t in, uint32_t out, uint16_t volume,
                    Bus& main) {
    for (unsigned ch = 0; ch < kChannelsPerBus; ++ch)
        for (uint32_t i = 0; i < kFrameSamples; ++i) {
            const uint32_t at = 4 * (ch * kFrameSamples + i);
            poke32(in + at, uint32_t(bus[ch][i]));
            main[ch][i] += int32_t((int64_t(int32_t(peek32(out + at))) * volume) >> 15);
        }
}

// An address in the command list (two halves, physical): a buffer in MEM1 or
// MEM2. Zero, or one outside guest memory, stops rather than becoming a
// write into low memory.
uint32_t list_addr(uint32_t at, uint32_t list) {
    const uint32_t phys = half_pair(at);
    if (phys == 0 || !guest_range_ok(phys | 0x80000000u, 4)) unsupported("an address in the command list", list);
    return phys | 0x80000000u;
}
// One frame. AX's command list (16-bit words; addresses as two halves):
//   0 studio | 1, 2, 3 mix buffer (by output mode) | 4 first PB
//   5, 6, 7 aux A, B, C: return volume, input, output
//   8, 9 aux A, B for Dolby Pro Logic II | 10 compressor: 0x8000, value, table
//   11 output: master volume, mix buffer, the AI buffer | 12 the same, DPL2
//   13 the four remote speakers' buffers | 14 end
void run_frame(uint32_t list) {
    Bus buses[kBuses] = {};
    uint32_t aux_in[kBuses] = {}, aux_out[kBuses] = {};
    uint16_t aux_volume[kBuses] = {};
    uint32_t output = 0, remotes[4] = {};
    uint16_t master = 0;
    uint32_t first_pb = 0;
    uint32_t a = list;
    for (unsigned n = 0;; ++n) {
        if (n > 64) unsupported("a command list with no end", list);
        const uint16_t cmd = peek16(a);
        a += 2;
        if (cmd == 14) break;
        switch (cmd) {
        case 0: case 1: a += 4; break;  // the studio (depop, kept by AX) and the mix buffer
        case 4: first_pb = list_addr(a, list), a += 4; break;
        case 5: case 6: case 7: {
            const unsigned bus = cmd - 4u;
            aux_volume[bus] = peek16(a);
            aux_in[bus] = list_addr(a + 2, list), aux_out[bus] = list_addr(a + 6, list);
            a += 10;
            break;
        }
        case 10: a += 8; break;  // APPROX: the compressor is saturation at the output
        case 11:
            master = peek16(a);
            output = list_addr(a + 6, list);
            a += 10;
            break;
        case 13:
            for (unsigned r = 0; r < 4; ++r) remotes[r] = list_addr(a + 4 * r, list);
            a += 16;
            break;
        case 2: case 3: case 8: case 9: case 12: unsupported("surround or Dolby Pro Logic II output", list);
        default:
            std::fprintf(stderr, "qilin_run: DSP: command %u in the list at %08X\n", cmd, list);
            throw Stop{Stop::Unsupported, 0, a - 2};
        }
    }
    if (!first_pb || !output) unsupported("a command list without voices or output", list);

    uint32_t p = first_pb;
    for (unsigned n = 0; p != 0x80000000u; ++n) {
        if (n > 256) unsupported("a voice chain that doesn't end", first_pb);
        if (peek16(p + pb::kState) == pb::kRunning) mix_voice(p, buses);
        p = half_pair(p + pb::kNext) | 0x80000000u;
    }
    for (unsigned bus = 1; bus < kBuses; ++bus)
        if (aux_in[bus]) aux_round_trip(buses[bus], aux_in[bus], aux_out[bus], aux_volume[bus], buses[0]);

    // Stereo output: L and R with the master volume. The surround channel has
    // no stereo fold-down with a source yet, so it stops when used.
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
        if (buses[0][2][i] != 0) unsupported("the surround channel in stereo output", first_pb);
        poke16(output + 4 * i, uint16_t(clamp16((int64_t(buses[0][0][i]) * master) >> 15)));
        poke16(output + 4 * i + 2, uint16_t(clamp16((int64_t(buses[0][1][i]) * master) >> 15)));
    }
    // The remote speakers: silent (no remote speaker output yet; see wpad.cpp).
    constexpr uint32_t kRemoteSamples = 18;  // 6 kHz
    for (uint32_t r : remotes)
        if (r)
            for (uint32_t i = 0; i < kRemoteSamples; ++i) poke16(r + 2 * i, 0);
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
