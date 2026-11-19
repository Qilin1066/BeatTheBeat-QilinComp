// Dolby Pro Logic II encoding for the DSP mixer (audio.cpp): L, R, Ls and Rs
// matrixed into the two channels the console outputs, for a decoder to take
// apart again:
//
//   Lt = L - j(a Ls + b Rs),  Rt = R + j(b Ls + a Rs),  a = 0.8718, b = 0.4899
//
// (the published Pro Logic II matrix; a^2 + b^2 = 1). APPROX: the DSP's own
// arithmetic for it (its filters, its scaling) has no source.
//
// The 90-degree shift is a pair of all-pass chains: the front goes through
// chain A, the surround sums through chain B, which lags A by 90 degrees
// (within 1.4 degrees from 20 Hz to 15 kHz at 32 kHz). Each section is
// (c - z^-2) / (1 - c z^-2), so the gain is exactly 1 at every frequency;
// chain B has one more sample of delay. The coefficients are this project's
// own design: tools/analysis/design_dpl2_allpass.py. The state carries
// across frames. Header-only so the unit tests check it. Runtime code: no
// std::format.
#pragma once

#include <cstdint>

namespace qilin::rt {

template <uint32_t kFrame>
class Dpl2Encoder {
public:
    void reset() { *this = Dpl2Encoder(); }

    // One frame of L, R, Ls, Rs to Lt and Rt.
    void encode(const int32_t* l, const int32_t* r, const int32_t* ls, const int32_t* rs, float* lt, float* rt) {
        for (uint32_t i = 0; i < kFrame; ++i) {
            const float sl = float(ls[i]), sr = float(rs[i]);
            const float front_l = a_l_.run(float(l[i]), kChainA), front_r = a_r_.run(float(r[i]), kChainA);
            const float back_l = b_l_.run(delay(delay_l_, kA * sl + kB * sr), kChainB);
            const float back_r = b_r_.run(delay(delay_r_, kB * sl + kA * sr), kChainB);
            lt[i] = front_l + back_l;  // B is -j against A
            rt[i] = front_r - back_r;
        }
    }

private:
    static constexpr int kSections = 4;
    static constexpr float kChainA[kSections] = {0.2011009966f, 0.8015925431f, 0.9669026875f, 0.9913530831f};
    static constexpr float kChainB[kSections] = {0.5575006245f, 0.9188802207f, 0.9843750958f, 0.9970247226f};
    static constexpr float kA = 0.8718f, kB = 0.4899f;

    // A cascade of (c - z^-2) / (1 - c z^-2): y[n] = c (x[n] + y[n-2]) - x[n-2].
    struct Chain {
        float x1[kSections] = {}, x2[kSections] = {}, y1[kSections] = {}, y2[kSections] = {};
        float run(float x, const float (&c)[kSections]) {
            for (int s = 0; s < kSections; ++s) {
                const float y = c[s] * (x + y2[s]) - x2[s];
                x2[s] = x1[s], x1[s] = x;
                y2[s] = y1[s], y1[s] = y;
                x = y;
            }
            return x;
        }
    };
    static float delay(float& last, float x) {
        const float out = last;
        last = x;
        return out;
    }

    Chain a_l_, a_r_, b_l_, b_r_;
    float delay_l_ = 0, delay_r_ = 0;
};

}  // namespace qilin::rt
