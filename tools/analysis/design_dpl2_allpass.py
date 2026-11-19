#!/usr/bin/env python3
"""Designs the all-pass pair the runtime's Pro Logic II encoder uses
(recomp/runtime/dpl2.hpp): two chains whose outputs differ in phase by 90
degrees across the audio band, so the surround channels can be matrixed in
at -j and +j relative to the front.

Each chain is a cascade of sections H(z) = (a - z^-2) / (1 - a z^-2), all-pass
for any 0 <= a < 1, so the gain is exactly 1 at every frequency; chain B has
one more sample of delay. The coefficients are found here by minimising the
worst phase error against 90 degrees over 20 Hz .. 15 kHz at 32 kHz
(Nelder-Mead on a smoothed maximum, from a spread start, then refined).
Pure Python, no dependencies:

    python3 tools/analysis/design_dpl2_allpass.py

prints the coefficients and the worst error; dpl2.hpp has the result.
"""
import cmath
import math

FS = 32000.0
SECTIONS = 4
FREQS = [20.0 * (15000.0 / 20.0) ** (i / 299.0) for i in range(300)]  # log-spaced


def phase(coefs, w, delay):
    z2 = cmath.exp(-2j * w)
    h = cmath.exp(-1j * w * delay)
    for a in coefs:
        h *= (a - z2) / (1 - a * z2)
    return cmath.phase(h)


def errors(p):
    a, b = p[:SECTIONS], p[SECTIONS:]
    out = []
    for f in FREQS:
        w = 2 * math.pi * f / FS
        d = phase(a, w, 0) - phase(b, w, 1)
        d = (d + math.pi) % (2 * math.pi) - math.pi  # wrap to (-pi, pi]
        out.append(abs(abs(d) - math.pi / 2))
    return out


def cost(p, power):
    if any(not (0.0 <= x < 0.99999) for x in p):
        return 1e9
    e = errors(p)
    return (sum(x ** power for x in e) / len(e)) ** (1.0 / power)


def nelder_mead(f, x0, step, iters):
    n = len(x0)
    pts = [list(x0)] + [[x0[j] + (step if i == j else 0.0) for j in range(n)] for i in range(n)]
    vals = [f(p) for p in pts]
    for _ in range(iters):
        order = sorted(range(n + 1), key=lambda i: vals[i])
        pts, vals = [pts[i] for i in order], [vals[i] for i in order]
        centre = [sum(p[j] for p in pts[:-1]) / n for j in range(n)]
        refl = [centre[j] + (centre[j] - pts[-1][j]) for j in range(n)]
        fr = f(refl)
        if fr < vals[0]:
            exp = [centre[j] + 2 * (centre[j] - pts[-1][j]) for j in range(n)]
            fe = f(exp)
            pts[-1], vals[-1] = (exp, fe) if fe < fr else (refl, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = refl, fr
        else:
            con = [centre[j] + 0.5 * (pts[-1][j] - centre[j]) for j in range(n)]
            fc = f(con)
            if fc < vals[-1]:
                pts[-1], vals[-1] = con, fc
            else:
                for i in range(1, n + 1):
                    pts[i] = [pts[0][j] + 0.5 * (pts[i][j] - pts[0][j]) for j in range(n)]
                    vals[i] = f(pts[i])
    best = min(range(n + 1), key=lambda i: vals[i])
    return pts[best]


def main():
    # Interleaved poles spread over the band, a's and b's alternating.
    spread = [1.0 - 2.0 ** -(1.0 + 1.6 * k) for k in range(2 * SECTIONS)]
    p = spread[0::2] + spread[1::2]
    for power, iters in ((4, 3000), (16, 3000), (64, 4000)):
        p = nelder_mead(lambda q: cost(q, power), p, 0.02, iters)
    a, b = sorted(p[:SECTIONS]), sorted(p[SECTIONS:])
    worst = max(errors(a + b))
    print("chain A (no delay):", ", ".join("%.10f" % x for x in a))
    print("chain B (one sample):", ", ".join("%.10f" % x for x in b))
    print("worst phase error over 20 Hz - 15 kHz: %.3f degrees" % math.degrees(worst))


if __name__ == "__main__":
    main()
