#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""STRANGE's built-in samples, synthesized here (no recordings, nothing third-party), as a C header in the shape of
gen_samples.py's (SMP_DATA, SMP_ZONES, SMP_SETS, ...): eng_sample.c includes it instead in the STRANGE build.

  sets (GRAIN's SRC; SAMPLE's SET, hidden in STRANGE), 22,050 Hz; the indices as in Felucca (1 and 4 aliases of 0),
  so USR1..3 stay SRC 5..7:
    0 GLASS   bowed glass: five inharmonic partials, each a slowly beating pair, a breath of bow noise (1.2 s, A3)
    2 THROAT  a throat drone: a buzzing source through three formants gliding from "a" to "o" (1.2 s, A2)
    3 METAL   a struck plate: eight inharmonic modes, each its own decay (1.2 s, D4)
  the GLITCH kit (eng_glitch.c), 11,025 Hz: 16 slices of broken digital sound, C3 .. D#4 (GLT_ZONES, after the sets)

  gen_strange_samples.py OUT.h
Pure Python (math, random with fixed seeds): the same bytes on every run.
"""
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sampleio import ima_encode  # noqa: E402

TAU = 2 * math.pi


def norm(x, peak=30000):
    m = max(1e-9, max(abs(v) for v in x))
    return [max(-32768, min(32767, int(v / m * peak))) for v in x]


def fade(x, a=64, b=256):
    n = len(x)
    for i in range(min(a, n)):
        x[i] *= i / a
    for i in range(min(b, n)):
        x[n - 1 - i] *= i / b
    return x


def onepole(x, k):
    y, out = 0.0, []
    for v in x:
        y += k * (v - y)
        out.append(y)
    return out


# ----------------------------------------------------------------- GRAIN's sources (22,050 Hz) ---
SR = 22050


def glass():
    rnd = random.Random(11)
    n, f0 = int(SR * 1.2), 220.0
    parts = [(1.0, 1.0), (2.756, 0.5), (5.404, 0.33), (8.933, 0.2), (13.34, 0.11)]
    noise = onepole([rnd.uniform(-1, 1) for _ in range(n)], 0.08)
    x = []
    ph = [[rnd.random() * TAU, rnd.random() * TAU] for _ in parts]
    for i in range(n):
        t = i / SR
        s = 0.0
        for k, (r, a) in enumerate(parts):
            f = f0 * r
            beat = 0.25 + 0.15 * k
            s += a * (math.sin(TAU * f * t + ph[k][0]) + math.sin(TAU * (f + beat) * t + ph[k][1])) * 0.5
        swell = 0.7 + 0.3 * math.sin(TAU * 0.6 * t)
        x.append(s * swell + 0.05 * noise[i])
    return fade(x, 400, 400)


def throat():
    rnd = random.Random(23)
    n, f0 = int(SR * 1.2), 110.0
    src, ph = [], 0.0
    for i in range(n):
        f = f0 * (1 + 0.004 * math.sin(TAU * 5.1 * i / SR))
        ph = (ph + f / SR) % 1.0
        src.append((2 * ph - 1) * 0.7 + 0.15 * rnd.uniform(-1, 1))
    out = [0.0] * n
    fa, fo = [(730, 90), (1090, 110), (2440, 160)], [(570, 80), (840, 100), (2410, 150)]
    for k in range(3):
        y1 = y2 = 0.0
        for i in range(n):
            m = i / n
            f = fa[k][0] * (1 - m) + fo[k][0] * m
            bw = fa[k][1] * (1 - m) + fo[k][1] * m
            r = math.exp(-math.pi * bw / SR)
            c1, c2 = 2 * r * math.cos(TAU * f / SR), -r * r
            y = (1 - r) * src[i] + c1 * y1 + c2 * y2
            y2, y1 = y1, y
            out[i] += y * (1.0, 0.6, 0.3)[k]
    return fade(out, 600, 600)


def metal():
    rnd = random.Random(37)
    n, f0 = int(SR * 1.2), 293.66
    modes = [(1.0, 1.0, 1.2), (1.594, 0.8, 0.9), (2.136, 0.6, 0.8), (2.653, 0.55, 0.6), (3.156, 0.4, 0.5),
             (3.652, 0.32, 0.4), (4.06, 0.25, 0.35), (4.602, 0.2, 0.3)]
    ph = [rnd.random() * TAU for _ in modes]
    x = []
    for i in range(n):
        t = i / SR
        s = sum(a * math.exp(-t / d) * math.sin(TAU * f0 * r * t + ph[k]) for k, (r, a, d) in enumerate(modes))
        x.append(s)
    return fade(x, 8, 800)


# ------------------------------------------------------------------- the GLITCH kit (11,025 Hz) ---
GR = 11025
SL = 1875                                            # 0.17 s a slice


def crush(x, bits, hold):
    q = 2 ** (bits - 1)
    out, v = [], 0.0
    for i, s in enumerate(x):
        if i % hold == 0:
            v = round(s * q) / q
        out.append(v)
    return out


def glitch_kit():
    rnd = random.Random(101)
    t = [i / GR for i in range(SL)]
    env = lambda d: [math.exp(-i / (GR * d)) for i in range(SL)]   # noqa: E731
    sl = []
    # 1 crushed noise burst
    sl.append([v * e for v, e in zip(crush([rnd.uniform(-1, 1) for _ in range(SL)], 3, 9), env(0.05))])
    # 2 an accelerating click train
    x, p, gap = [0.0] * SL, 0, 300
    while p < SL:
        for k in range(6):
            if p + k < SL:
                x[p + k] = (1, -0.6, 0.4, -0.2, 0.1, -0.05)[k]
        p += max(18, int(gap))
        gap *= 0.8
    sl.append(x)
    # 3 a falling FM chirp
    x, ph, mp = [], 0.0, 0.0
    for i in range(SL):
        f = 2400 * math.exp(-i / 500)
        mp += 3.1 * f / GR
        ph += f * (1 + 2.5 * math.sin(TAU * mp)) / GR
        x.append(math.sin(TAU * ph) * math.exp(-i / 1200))
    sl.append(x)
    # 4 stutter: a tone chopped, its pitch jumping
    x = []
    for i in range(SL):
        seg = i // 220
        f = (440, 660, 330, 880, 495, 220, 990, 587, 370)[seg % 9]
        x.append(math.sin(TAU * f * t[i]) * (1 if (i % 220) < 150 else 0))
    sl.append(x)
    # 5 dropouts: a chord with holes (an underrun)
    holes = set()
    for _ in range(9):
        a = rnd.randrange(SL)
        holes.update(range(a, a + rnd.randrange(30, 140)))
    sl.append([0 if i in holes else (math.sin(TAU * 220 * t[i]) + math.sin(TAU * 277 * t[i]) + math.sin(TAU * 330 * t[i])) / 3
               for i in range(SL)])
    # 6 a ring-modulated zap
    sl.append([math.sin(TAU * (90 + 4000 * t[i]) * t[i]) * math.sin(TAU * 1730 * t[i]) * math.exp(-i / 900) for i in range(SL)])
    # 7 a tone decimated ever more (a sample-rate sweep)
    x, v = [], 0.0
    for i in range(SL):
        h = 1 + i // 90
        if i % h == 0:
            v = math.sin(TAU * 523 * t[i]) + 0.5 * math.sin(TAU * 1046 * t[i])
        x.append(v * 0.7)
    sl.append(x)
    # 8 crackle: sparse impulses of random sign
    x = [0.0] * SL
    for _ in range(40):
        p = rnd.randrange(SL - 4)
        a = rnd.uniform(0.3, 1) * rnd.choice((-1, 1))
        x[p], x[p + 1], x[p + 2] = a, -0.5 * a, 0.2 * a
    sl.append(x)
    # 9 modem: FSK tones flipping
    x, ph = [], 0.0
    for i in range(SL):
        f = 1200 if (i // 37) % 3 else 2200
        ph += f / GR
        x.append(1.0 if (ph % 1) < 0.5 else -1.0)
    sl.append([v * 0.5 for v in onepole(x, 0.5)])
    # 10 a clipped thump
    x, ph = [], 0.0
    for i in range(SL):
        f = 40 + 120 * math.exp(-i / 180)
        ph += f / GR
        x.append(max(-0.6, min(0.6, 1.6 * math.sin(TAU * ph) * math.exp(-i / 1400))))
    sl.append(x)
    # 11 a CD skip: one fragment again and again
    frag = [math.sin(TAU * 311 * i / GR) * 0.6 + rnd.uniform(-0.3, 0.3) for i in range(150)]
    sl.append([frag[i % 150] * (1 if (i // 150) % 4 != 3 else 0.3) for i in range(SL)])
    # 12 data: an LFSR's bytes as sound, its taps changing
    x, s = [], 0xACE1
    for i in range(SL):
        b = ((s >> 0) ^ (s >> 2) ^ (s >> 3) ^ (s >> (5 if i < SL // 2 else 7))) & 1
        s = (s >> 1) | (b << 15)
        x.append(((s & 0xFF) - 128) / 128)
    sl.append([v * e for v, e in zip(x, env(0.12))])
    # 13 a beep, square, with a fault in the middle
    sl.append([(0.5 if (int(1000 * t[i] * 2) % 2) else -0.5) * (0 if 800 < i < 1000 else 1) * (i < 1500) for i in range(SL)])
    # 14 a metallic tick
    sl.append([sum(math.sin(TAU * f * t[i]) for f in (3170, 4390, 5830, 7210)) * math.exp(-i / 160) for i in range(SL)])
    # 15 a reversed swell, cut
    n = onepole([rnd.uniform(-1, 1) for _ in range(SL)], 0.3)
    sl.append([n[i] * math.exp((i - SL) / 400) * (i < SL - 60) for i in range(SL)])
    # 16 a smear of tiny grains, random pitches
    x = [0.0] * SL
    for _ in range(28):
        p, f, L = rnd.randrange(SL - 200), rnd.uniform(300, 2500), rnd.randrange(60, 200)
        for k in range(L):
            x[p + k] += math.sin(TAU * f * k / GR) * math.sin(math.pi * k / L) * 0.5
    sl.append(x)
    return [fade(list(s), 8, 40) for s in sl]


# --------------------------------------------------------------------------------------- header ---
def main(out):
    data, zones = bytearray(), []

    def add(x, rate, root, lo, hi):
        s = norm(x)
        enc, (pred, idx) = ima_encode(s, 0)
        zones.append((len(data), len(s), 0, len(s) - 1, round(rate / 44100 * 65536), root * 16, pred, idx, lo, hi, 0))
        data.extend(enc)

    add(glass(), SR, 57, 0, 127)
    add(throat(), SR, 45, 0, 127)
    add(metal(), SR, 62, 0, 127)
    z_glitch = len(zones)
    for k, x in enumerate(glitch_kit()):
        add(x, GR, 48 + k, 48 + k, 48 + k)
    L = ["/* generated by tools/gen_strange_samples.py: STRANGE's built-in samples (synthesized) */", "#pragma once",
         f"static const uint8_t SMP_DATA[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        L.append("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",")
    L.append("};")
    L.append("static const smp_zone_t SMP_ZONES[] = {")
    for z in zones[:z_glitch]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L.append("};")
    L += ['static const smp_set_t SMP_SETS[] = {', '    {"GLASS", 0, 1},', '    {"GLASS", 0, 1},', '    {"THROAT", 1, 1},',
          '    {"METAL", 2, 1},', '    {"GLASS", 0, 1},', '};', '#define SMP_NSETS 5', '#define SMP_NPRESETS 4',
          '#define SMP_PERC_SLOT 4   /* (an alias, as in Felucca: the indices stay) */',
          'static const preset_t SMP_PRESET_TABLE[] = {',
          '    {"GLASS", {0, 0, 0, 1, 127, 0, 0, 0}, {40, 127, 127, 90}, 0, 0},',
          '    {"GLASS", {0, 0, 0, 1, 127, 0, 0, 0}, {40, 127, 127, 90}, 0, 0},',
          '    {"THROAT", {2, 0, 0, 1, 127, 0, 0, 0}, {40, 127, 127, 90}, 0, 0},',
          '    {"METAL", {3, 0, 0, 1, 127, 0, 0, 0}, {0, 127, 127, 90}, 0, 0},',
          '    {"GLASS", {0, 0, 0, 1, 127, 0, 0, 0}, {40, 127, 127, 90}, 0, 0},', '};',
          'static const uint8_t SMP_SET_ORIG[] = {0, 0, 2, 3, 0};', '#define SMP_NALIAS 1',
          '#define SMP_SET_NAMES_INIT "GLASS", "GLASS", "THROAT", "METAL", "GLASS"',
          'static const char *const SMP_SET_NAMES[] = {"GLASS", "GLASS", "THROAT", "METAL", "GLASS"};', '',
          f'/* the GLITCH kit (eng_glitch.c): {len(zones) - z_glitch} slices, notes 48 .. {48 + len(zones) - z_glitch - 1}, 11,025 Hz */',
          f'#define GLT_NZ {len(zones) - z_glitch}', 'static const smp_zone_t GLT_ZONES[] = {']
    for z in zones[z_glitch:]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L += ["};", ""]
    Path(out).write_text("\n".join(L))
    print(f"strange samples: {len(data)} B ({len(zones)} zones: 3 sources, {len(zones) - z_glitch} glitch slices)")


if __name__ == "__main__":
    main(sys.argv[1])
