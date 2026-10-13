#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""STRANGE's built-in samples, synthesized here (no recordings, nothing third-party), as a C header in the shape of
gen_samples.py's (SMP_DATA, SMP_ZONES, SMP_SETS, ...): eng_sample.c includes it instead in the STRANGE build.

  sets (GRAIN's SRC; SAMPLE's SET, hidden in STRANGE), 22,050 Hz; the indices as in Felucca (1 and 4 aliases of 0),
  so USR1..3 stay SRC 5..7:
    0 GLASS   bowed glass: five inharmonic partials, each a slowly beating pair, a breath of bow noise (1.2 s, A3)
    2 THROAT  a throat drone: a buzzing source through three formants gliding from "a" to "o" (1.2 s, A2)
    3 METAL   a struck plate: eight inharmonic modes, each its own decay (1.2 s, D4)
  the GLITCH kit (eng_glitch.c), 22,050 Hz: 16 slices, C3 .. D#4: glass, short-wave air, neon and current (GLT_ZONES)

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



# ------------------------------------------------- the GLITCH kit (22,050 Hz): glass, short-wave air, current ---
GR = 22050


def N(sec):
    return int(sec * GR)


def hp(x, k):
    lp = onepole(x, k)
    return [a - b for a, b in zip(x, lp)]


def bp(x, f, q):                                        # a biquad band-pass
    w = TAU * f / GR
    al = math.sin(w) / (2 * q)
    a0, a1, a2 = 1 + al, -2 * math.cos(w), 1 - al
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for v in x:
        y = (al * v - al * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1 = x1, v
        y2, y1 = y1, y
        out.append(y)
    return out


def modes(n, parts, rnd):
    x = [0.0] * n
    for f, a, d in parts:
        ph = rnd.uniform(0, TAU)
        for i in range(n):
            x[i] += a * math.sin(TAU * f * i / GR + ph) * math.exp(-i / (GR * d))
    return x


# --- GLASS ---
def glass_tap():
    n=N(0.45); f0=1840
    return modes(n,[(f0,1,0.25),(f0*2.32,0.6,0.16),(f0*4.25,0.4,0.08),(f0*6.63,0.25,0.05),(f0*2.36,0.3,0.2)],R)
def glass_crack():
    n=N(0.4); x=[0.0]*n; t=0
    while t<n:
        a=math.exp(-t/(GR*0.12))*R.uniform(0.3,1)
        f=R.uniform(2500,7000); L=R.randrange(20,90)
        for k in range(L):
            if t+k<n: x[t+k]+=a*math.sin(TAU*f*k/GR)*math.exp(-k/12)
        t+=int(R.expovariate(1/ (40+t*0.02)))+3
    return x
def glass_rub():
    n=N(0.5); f=880; x=[]
    for i in range(n):
        tt=i/GR; am=0.6+0.4*math.sin(TAU*6.5*tt)
        x.append(am*(math.sin(TAU*f*tt)+0.4*math.sin(TAU*f*1.004*tt)+0.25*math.sin(TAU*f*2.74*tt))*(1-math.exp(-tt*30)))
    nz=bp([R.uniform(-1,1) for _ in range(n)],f,8)
    return [a+0.6*b for a,b in zip(x,nz)]
def glass_shards():
    n=N(0.5); x=[0.0]*n
    for k in range(34):
        t=int(n*(R.random()**1.8)); f=R.uniform(2000,6500); a=R.uniform(0.2,1)*(1-t/n)
        for j in range(min(600,n-t)): x[t+j]+=a*(math.sin(TAU*f*j/GR)+0.5*math.sin(TAU*f*2.7*j/GR))*math.exp(-j/(GR*0.012))
    return x
# --- ETHER (shortwave) ---
def sw_tune():   # heterodyne whistle sweeping through zero beat, in static
    n=N(0.5); x=[]; ph=0
    for i in range(n):
        tt=i/GR; f=2600*(1-2*tt/0.5); ph+=abs(f)/GR
        x.append(0.6*math.sin(TAU*ph))
    nz=hp([R.uniform(-1,1) for _ in range(n)],0.3)
    return [a+0.35*b for a,b in zip(x,nz)]
def sw_clicks():  # complex-modulated clicks: data bursts with jittering rate, AM fading, pitch per burst
    n=N(0.45); x=[0.0]*n; t=0; rate=120
    while t<n:
        fade_=0.5+0.5*math.sin(TAU*3.2*t/GR)
        pitch=R.choice((700,1100,1500,2100))
        L=R.randrange(10,40)
        for k in range(L):
            if t+k<n: x[t+k]+=fade_*math.sin(TAU*pitch*k/GR)*math.exp(-k/6)
        rate=max(14,min(400,rate*R.choice((0.7,1,1.35))))
        t+=int(rate)+R.randrange(0,20)
    return bp(x,1300,0.8)
def sw_static():  # spherics: crackle through a band, sweeping
    n=N(0.5); x=[0.0]*n
    for _ in range(70):
        p=R.randrange(n-3); a=R.uniform(0.2,1)*R.choice((-1,1)); x[p]+=a; x[p+1]-=0.5*a
    y=[]; lo=0; 
    for i,v in enumerate(x):
        y.append(v)
    return [a+0.15*b for a,b in zip(bp(y,900,0.7), [R.uniform(-1,1) for _ in range(n)])]
def sw_ssb():   # a "duck" voice: buzz through gliding formants, frequency-shifted
    n=N(0.5); src=[]; ph=0
    for i in range(n):
        f0=140+40*math.sin(TAU*3*i/GR); ph+=f0/GR; src.append(2*(ph%1)-1)
    v=[0.0]*n
    for (f1,f2,a) in ((600,1200,1),(400,2000,0.7)):
        pass
    out=[]
    a=bp(src,700,5); b=bp(src,1250,6); c=bp(src,2400,7)
    for i in range(n):
        tt=i/GR; out.append((a[i]+0.7*b[i]*(0.5+0.5*math.sin(TAU*5*tt))+0.4*c[i]))
    # crude frequency shift by ring-mod with 230 Hz + lowpass mix
    out=[o*math.cos(TAU*230*i/GR)+0.5*o for i,o in enumerate(out)]
    return out
def sw_cw():    # morse with fading and a second station
    n=N(0.5); x=[]; key="10111011101000101110"; dot=int(GR*0.022)
    for i in range(n):
        k=key[(i//dot)%len(key)]=="1"; fd=0.55+0.45*math.sin(TAU*4.1*i/GR)
        k2=((i//int(GR*0.031))%5)<2
        x.append(fd*0.7*k*math.sin(TAU*760*i/GR)+0.25*k2*math.sin(TAU*1130*i/GR))
    return [a+0.12*R.uniform(-1,1) for a in onepole(x,0.5)]
def rtty():     # FSK burst, 45-ish baud, hard
    n=N(0.4); x=[]; ph=0; bit=int(GR/120)
    for i in range(n):
        f=2125 if random.Random((i//bit)*7).random()<0.5 else 2295
        ph+=f/GR; x.append(0.6 if (ph%1)<0.5 else -0.6)
    return onepole(x,0.4)
# --- CURRENT (neon, choke, arcs) ---
def neon_start():  # relaxation oscillator: pulses accelerate as the choke charges, then a buzzing glow
    n=N(0.6); x=[0.0]*n; t=0; per=int(GR/6)
    while t<n:
        for k in range(160):
            if t+k<n: x[t+k]+=math.sin(TAU*3100*k/GR)*math.exp(-k/18)+0.6*math.exp(-k/40)
        per=max(int(GR/1400), int(per*0.82)); t+=per+R.randrange(0,3)
    hum=[0.25*math.sin(TAU*100*i/GR)+0.12*math.sin(TAU*200*i/GR) for i in range(n)]
    return [a+b*(i/n) for i,(a,b) in enumerate(zip(x,hum))]
def choke_hum():  # magnetostriction: 100 Hz, rich, with flicker
    n=N(0.6); x=[]
    for i in range(n):
        tt=i/GR; s=math.sin(TAU*100*tt); v=math.copysign(abs(s)**0.25,s)
        fl=1 if random.Random(int(tt*60)).random()>0.12 else 0.3
        x.append(0.6*v*fl+0.15*math.sin(TAU*1000*tt)*fl)
    return onepole(x,0.6)
def arc():
    n=N(0.45); x=[]
    for i in range(n):
        tt=i/GR; on=random.Random(int(tt*90)).random()>0.35
        x.append((R.uniform(-1,1)*0.8+0.4*math.copysign(1,math.sin(TAU*100*tt)))*on)
    return hp(onepole(x,0.7),0.05)
def relay():   # a relay: the click, contact bounce, a buzz
    n=N(0.3); x=[0.0]*n
    for k,(t,a) in enumerate(((0,1),(180,0.5),(300,0.35),(390,0.2),(450,0.12))):
        for j in range(300):
            if t+j<n: x[t+j]+=a*math.sin(TAU*(2400-k*200)*j/GR)*math.exp(-j/25)
    for i in range(800,n): x[i]+=0.2*math.copysign(1,math.sin(TAU*100*i/GR))*math.exp(-(i-800)/3000)
    return x
def tube_warm():   # a valve warming: hum rising into a whine, crackle
    n=N(0.6); x=[]; ph=0
    for i in range(n):
        tt=i/GR; f=60*math.exp(tt*5); ph+=f/GR
        x.append(0.6*math.sin(TAU*ph)*(tt/0.6)+0.3*math.sin(TAU*100*tt))
        if R.random()<0.002: x[-1]+=R.uniform(-1,1)
    return x
def degauss():   # a CRT degauss: a 50 Hz thump, buzz, decaying wobble
    n=N(0.6); x=[]
    for i in range(n):
        tt=i/GR; e=math.exp(-tt*5)
        s=math.sin(TAU*50*tt); x.append(e*(0.9*math.copysign(abs(s)**0.4,s)+0.3*math.sin(TAU*150*tt*(1+0.02*math.sin(TAU*7*tt)))))
    return onepole(x,0.5)


def glitch_kit():
    """16 slices, C3 .. D#4: GLASS TAP, CRACK, RUB, SHARDS; SW TUNE (a heterodyne through zero beat), SW CLICKS (data
    bursts, their rate, pitch and fading jumping), SW STATIC, SW SSB (a duck voice), SW CW, RTTY; NEON START (a
    relaxation oscillator speeding up on its choke), CHOKE HUM, ARC, RELAY, TUBE WARM, DEGAUSS"""
    global R
    R = random.Random(7)
    fs = (glass_tap, glass_crack, glass_rub, glass_shards, sw_tune, sw_clicks, sw_static, sw_ssb, sw_cw, rtty,
          neon_start, choke_hum, arc, relay, tube_warm, degauss)
    return [fade(f(), 16, 300) for f in fs]


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
          f'/* the GLITCH kit (eng_glitch.c): {len(zones) - z_glitch} slices, notes 48 .. {48 + len(zones) - z_glitch - 1}, 22,050 Hz */',
          f'#define GLT_NZ {len(zones) - z_glitch}', 'static const smp_zone_t GLT_ZONES[] = {']
    for z in zones[z_glitch:]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L += ["};", ""]
    Path(out).write_text("\n".join(L))
    print(f"strange samples: {len(data)} B ({len(zones)} zones: 3 sources, {len(zones) - z_glitch} glitch slices)")


if __name__ == "__main__":
    main(sys.argv[1])
