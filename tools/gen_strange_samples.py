#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""STRANGE's built-in samples, synthesized here (no recordings, nothing third-party), as a C header in the shape of
gen_samples.py's (SMP_DATA, SMP_ZONES, SMP_SETS, ...): eng_sample.c includes it instead in the STRANGE build.

  sets (GRAIN's SRC; SAMPLE's SET, hidden in STRANGE), 22,050 Hz; the indices as in Felucca (1 and 4 aliases of 0),
  so USR1..3 stay SRC 5..7:
    0 GLASS   bowed glass: five inharmonic partials, each a slowly beating pair, a breath of bow noise (0.8 s, A3)
    2 THROAT  a throat drone: a buzzing source through three formants gliding from "a" to "o" (0.8 s, A2)
    3 METAL   a struck plate: eight inharmonic modes, each its own decay (0.8 s, D4)
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
    n, f0 = int(SR * 0.8), 220.0
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
    n, f0 = int(SR * 0.8), 110.0
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
    n, f0 = int(SR * 0.8), 293.66
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


# ------------------------------------------- RITUAL and MECH kits (11,025 Hz): the two machines on GLITCH's engine ---
RG = 11025


def _n(sec):
    return int(sec * RG)


def _bp(x, f, q):
    w = TAU * f / RG
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


def _modes(n, parts, rnd):
    x = [0.0] * n
    for f, a, d in parts:
        if f >= RG / 2:
            continue
        ph = rnd.uniform(0, TAU)
        for i in range(n):
            x[i] += a * math.sin(TAU * f * i / RG + ph) * math.exp(-i / (RG * d))
    return x


def _noise(n, rnd):
    return [rnd.uniform(-1, 1) for _ in range(n)]


def _env(n, d):
    return [math.exp(-i / (RG * d)) for i in range(n)]


def _hits(n, times, fn):
    x = [0.0] * n
    for t, a in times:
        s = fn(a)
        for j, v in enumerate(s):
            if t + j < n:
                x[t + j] += v
    return x


def ritual_kit():
    """BELL, GONG, CHAINS, STONE GRIND, STONE KNOCK, CANDLE, CHOIR BREATH, CAVE DRIP, COFFIN, BONES, FRAME DRUM, BOWL,
    THROAT, CRYPT WIND, DOOR CREAK, TOLLING"""
    rnd = random.Random(31)
    k = []
    n = _n(0.5)                                                   # a small temple bell
    k.append(_modes(n, [(1210, 1, 0.3), (1210 * 2.76, 0.5, 0.15), (1210 * 5.4, 0.3, 0.06), (1214, 0.6, 0.3)], rnd))
    n = _n(0.55)                                                  # a gong: low partials, a shimmer rising after
    g = _modes(n, [(96, 1, 0.5), (151, 0.7, 0.4), (219, 0.5, 0.35), (313, 0.4, 0.3), (487, 0.3, 0.25)], rnd)
    sh = _bp(_noise(n, rnd), 1300, 2)
    k.append([a + 0.25 * b * min(1, i / (RG * 0.15)) * math.exp(-i / (RG * 0.3)) for i, (a, b) in enumerate(zip(g, sh))])
    n = _n(0.45)                                                  # chains: many small metal links knocking
    k.append(_hits(n, [(int(n * rnd.random() ** 1.5), rnd.uniform(0.2, 1)) for _ in range(40)],
                   lambda a: [a * math.sin(TAU * f * j / RG) * math.exp(-j / 40) for f in [rnd.uniform(1800, 4200)] for j in range(200)]))
    n = _n(0.45)                                                  # stone dragged on stone
    x = _bp(_noise(n, rnd), 380, 1.2)
    k.append([v * (0.4 + 0.6 * abs(math.sin(TAU * 7 * i / RG))) * (1 - math.exp(-i / 300)) for i, v in enumerate(x)])
    n = _n(0.3)                                                   # a stone knock
    k.append(_modes(n, [(420, 1, 0.03), (980, 0.6, 0.02), (1730, 0.4, 0.012)], rnd))
    n = _n(0.45)                                                  # a candle: soft sparse crackle over a breath
    c = _hits(n, [(rnd.randrange(n), rnd.uniform(0.2, 1)) for _ in range(18)],
              lambda a: [a * rnd.uniform(-1, 1) * math.exp(-j / 6) for j in range(30)])
    k.append([v + 0.05 * w for v, w in zip(c, _bp(_noise(n, rnd), 600, 0.8))])
    n = _n(0.5)                                                   # choir breath: an airy "ah"
    src = _noise(n, rnd)
    a = _bp(src, 700, 6); b = _bp(src, 1150, 7); c = _bp(src, 2600, 8)
    k.append([(a[i] + 0.7 * b[i] + 0.3 * c[i]) * math.sin(math.pi * i / n) for i in range(n)])
    n = _n(0.45)                                                  # a drip in a cave: the plop and its echoes
    def drip(a):
        return [a * math.sin(TAU * (900 + 1400 * math.exp(-j / 120)) * j / RG) * math.exp(-j / 260) for j in range(900)]
    k.append(_hits(n, [(0, 1), (int(RG * 0.11), 0.35), (int(RG * 0.23), 0.15), (int(RG * 0.34), 0.07)], drip))
    n = _n(0.45)                                                  # a coffin lid: a heavy wooden thud, a creak
    t = _modes(n, [(85, 1, 0.08), (190, 0.6, 0.05), (330, 0.3, 0.03)], rnd)
    cr = [0.25 * math.sin(TAU * (520 + 60 * math.sin(TAU * 13 * i / RG)) * i / RG) * (1 if (i // 90) % 2 else 0.4)
          * (i > RG * 0.18) * math.exp(-max(0, i - RG * 0.18) / (RG * 0.12)) for i in range(n)]
    k.append([a + b for a, b in zip(t, cr)])
    n = _n(0.4)                                                   # bones: dry clicks rattling
    k.append(_hits(n, [(int(n * (i / 14) ** 0.8) + rnd.randrange(0, 200), rnd.uniform(0.3, 1)) for i in range(14)],
                   lambda a: [a * math.sin(TAU * f * j / RG) * math.exp(-j / 18) for f in [rnd.uniform(1200, 2600)] for j in range(120)]))
    n = _n(0.4)                                                   # a frame drum, low
    f = [0.0] * n
    ph = 0.0
    for i in range(n):
        ph += (70 + 60 * math.exp(-i / 200)) / RG
        f[i] = math.sin(TAU * ph) * math.exp(-i / (RG * 0.12))
    k.append([a + 0.15 * b * math.exp(-i / 200) for i, (a, b) in enumerate(zip(f, _noise(n, rnd)))])
    n = _n(0.55)                                                  # a singing bowl struck
    k.append(_modes(n, [(520, 1, 0.5), (523, 0.8, 0.5), (1430, 0.4, 0.3), (1436, 0.3, 0.3), (2700, 0.2, 0.15)], rnd))
    n = _n(0.5)                                                   # a throat hum with its undertone
    x, ph = [], 0.0
    for i in range(n):
        ph += 110 / RG
        s = (2 * (ph % 1) - 1) + 0.6 * math.sin(TAU * ph / 2)
        x.append(s)
    a = _bp(x, 400, 4); b = _bp(x, 800, 5)
    k.append([(a[i] + 0.6 * b[i]) * min(1, i / 400) for i in range(n)])
    n = _n(0.5)                                                   # wind in a crypt: a band swelling, whistling
    x = _bp(_noise(n, rnd), 500, 3)
    k.append([v * math.sin(math.pi * i / n) ** 2 + 0.2 * math.sin(TAU * (640 + 40 * math.sin(TAU * 2 * i / RG)) * i / RG)
              * math.sin(math.pi * i / n) ** 3 for i, v in enumerate(x)])
    n = _n(0.45)                                                  # a door creaking: stick-slip
    x, ph, rate = [], 0.0, 30.0
    for i in range(n):
        rate = 30 + 40 * math.sin(TAU * 1.7 * i / RG) ** 2
        ph += rate / RG
        x.append((1 if (ph % 1) < 0.08 else 0) * rnd.uniform(0.6, 1))
    y = _bp(x, 900, 6)
    k.append([a + 0.5 * b for a, b in zip(y, _bp(x, 1700, 8))])
    n = _n(0.55)                                                  # tolling: a big bell, far away (dull)
    b = _modes(n, [(196, 1, 0.5), (392 * 1.19, 0.5, 0.4), (196 * 2.0, 0.6, 0.45), (196 * 3.0, 0.2, 0.2)], rnd)
    k.append(onepole(b, 0.5))
    return [fade(list(s), 8, 200) for s in k]


def mech_kit():
    """CLOCK TICK, TOCK, RATCHET, TYPEWRITER, CARRIAGE, REEL UP, TAPE STOP, PROJECTOR, PISTON, WIND-UP, SHUTTER, SERVO,
    HAMMER, CHAIN DRIVE, STEAM, MUSIC BOX"""
    rnd = random.Random(53)
    k = []
    n = _n(0.25)
    k.append(_modes(n, [(2400, 1, 0.006), (3900, 0.5, 0.004), (1700, 0.4, 0.01)], rnd))   # tick
    n = _n(0.3)
    k.append(_modes(n, [(900, 1, 0.012), (1600, 0.6, 0.008), (430, 0.5, 0.02)], rnd))     # tock
    n = _n(0.4)                                                   # a ratchet: pawl clicks speeding up
    times, t, gap = [], 0, 900
    while t < n:
        times.append((t, 1.0))
        t += int(gap)
        gap = max(160, gap * 0.85)
    k.append(_hits(n, times, lambda a: [a * math.sin(TAU * 3100 * j / RG) * math.exp(-j / 14) + 0.4 * a * math.exp(-j / 30) * rnd.uniform(-1, 1) for j in range(150)]))
    n = _n(0.25)                                                  # a typewriter key: the strike and the type bar
    k.append(_hits(n, [(0, 1), (int(RG * 0.012), 0.6), (int(RG * 0.06), 0.3)],
                   lambda a: [a * (rnd.uniform(-1, 1) * 0.6 + math.sin(TAU * 1400 * j / RG)) * math.exp(-j / 35) for j in range(300)]))
    n = _n(0.5)                                                   # the carriage: its ding and the return
    ding = _modes(n, [(2650, 0.8, 0.25), (2650 * 2.4, 0.3, 0.1)], rnd)
    ret = [0.4 * rnd.uniform(-1, 1) * (i > RG * 0.08) * math.exp(-max(0, i - RG * 0.08) / (RG * 0.15)) for i in range(n)]
    k.append([a + b for a, b in zip(ding, _bp(ret, 700, 1))])
    n = _n(0.5)                                                   # a reel spinning up: the motor whine rising, wow
    x, ph = [], 0.0
    for i in range(n):
        f = 60 + 500 * (i / n) ** 1.5
        ph += f / RG
        x.append(0.6 * math.sin(TAU * ph) + 0.3 * math.sin(TAU * 2 * ph) + 0.05 * rnd.uniform(-1, 1))
    k.append(x)
    n = _n(0.5)                                                   # tape stopping: a chord sliding down to nothing
    x, ph = [], 0.0
    for i in range(n):
        r = max(0.0, 1 - i / n) ** 1.6
        ph += 330 * r / RG
        x.append((math.sin(TAU * ph) + 0.5 * math.sin(TAU * ph * 1.5)) * r)
    k.append(x)
    n = _n(0.45)                                                  # a projector: the claw clattering at 24 frames
    k.append(_hits(n, [(int(i * RG / 24), 1) for i in range(12)],
                   lambda a: [a * rnd.uniform(-1, 1) * math.exp(-j / 25) + 0.3 * math.sin(TAU * 120 * j / RG) * math.exp(-j / 200) for j in range(400)]))
    n = _n(0.4)                                                   # a piston: the thump, the hiss
    th = _modes(n, [(60, 1, 0.06), (130, 0.5, 0.04)], rnd)
    hs = _bp(_noise(n, rnd), 3000, 1)
    k.append([a + 0.4 * b * (i > RG * 0.05) * math.exp(-max(0, i - RG * 0.05) / (RG * 0.1)) for i, (a, b) in enumerate(zip(th, hs))])
    n = _n(0.45)                                                  # winding up a spring: clicks, slowing
    times, t, gap = [], 0, 220
    while t < n:
        times.append((t, 0.7 + 0.3 * rnd.random()))
        t += int(gap)
        gap *= 1.12
    k.append(_hits(n, times, lambda a: [a * math.sin(TAU * 2200 * j / RG) * math.exp(-j / 10) for j in range(80)]))
    n = _n(0.3)                                                   # a shutter: two clacks and the blade's whirr
    k.append(_hits(n, [(0, 1), (int(RG * 0.07), 0.8)], lambda a: [a * rnd.uniform(-1, 1) * math.exp(-j / 20) for j in range(200)]))
    n = _n(0.45)                                                  # a servo: a geared whirr, up and back
    x, ph = [], 0.0
    for i in range(n):
        f = 700 + 400 * math.sin(math.pi * i / n)
        ph += f / RG
        x.append((0.5 if (ph % 1) < 0.3 else -0.2) * (0.7 + 0.3 * math.sin(TAU * 31 * i / RG)))
    k.append(onepole(x, 0.5))
    n = _n(0.4)                                                   # a hammer on an anvil
    k.append(_modes(n, [(1580, 1, 0.12), (3210, 0.6, 0.08), (4100, 0.4, 0.05), (870, 0.4, 0.1)], rnd))
    n = _n(0.45)                                                  # a chain drive: links over a sprocket
    k.append(_hits(n, [(int(i * RG / 38), 0.7 + 0.3 * (i % 3 == 0)) for i in range(17)],
                   lambda a: [a * math.sin(TAU * 1250 * j / RG) * math.exp(-j / 22) + 0.2 * a * rnd.uniform(-1, 1) * math.exp(-j / 10) for j in range(150)]))
    n = _n(0.45)                                                  # a steam valve: a burst of hiss, a whistle in it
    hs = _bp(_noise(n, rnd), 2600, 0.7)
    k.append([(a + 0.3 * math.sin(TAU * 1850 * i / RG)) * min(1, i / 120) * math.exp(-i / (RG * 0.2)) for i, a in enumerate(hs)])
    n = _n(0.55)                                                  # a music box: a comb's tines plucked
    x = [0.0] * n
    for t, f in ((0, 1568), (int(RG * 0.14), 1976), (int(RG * 0.28), 2349)):
        for j in range(min(int(RG * 0.4), n - t)):
            x[t + j] += (math.sin(TAU * f * j / RG) + 0.2 * math.sin(TAU * 2 * f * j / RG)) * math.exp(-j / (RG * 0.25))
    k.append(x)
    return [fade(list(s), 8, 200) for s in k]


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
    z_rit = len(zones)
    for k, x in enumerate(ritual_kit()):
        add(x, RG, 48 + k, 48 + k, 48 + k)
    z_mec = len(zones)
    for k, x in enumerate(mech_kit()):
        add(x, RG, 48 + k, 48 + k, 48 + k)
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
          f'/* the GLITCH kit (eng_glitch.c): {z_rit - z_glitch} slices, notes 48 .. {48 + z_rit - z_glitch - 1}, 22,050 Hz */',
          f'#define GLT_NZ {z_rit - z_glitch}', 'static const smp_zone_t GLT_ZONES[] = {']
    for z in zones[z_glitch:z_rit]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L += ["};", "/* RITUAL's and MECH's kits (eng_glitch.c, the same engine), 11,025 Hz */",
          f"#define RIT_NZ {z_mec - z_rit}", "static const smp_zone_t RIT_ZONES[] = {"]
    for z in zones[z_rit:z_mec]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L += ["};", f"#define MEC_NZ {len(zones) - z_mec}", "static const smp_zone_t MEC_ZONES[] = {"]
    for z in zones[z_mec:]:
        L.append("    {" + ", ".join(str(v) for v in z) + "},")
    L += ["};", ""]
    Path(out).write_text("\n".join(L))
    print(f"strange samples: {len(data)} B ({len(zones)} zones: 3 sources, 16 x 3 kit slices)")


if __name__ == "__main__":
    main(sys.argv[1])
