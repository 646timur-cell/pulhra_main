#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check 16/24-bit WAV recordings of PULHRA USB diagnostic builds.
Stereo: exact diagnostic triangles at 44.1/48 kHz. Current DRUMS counters are in channels 3/4 of 5 or 6, archived six-channel builds in 5/6;
a stereo recording of just the counter pair is also recognised. Exit 1 for corruption or unrecognised audio.
  python3 tools/uac_tone_check.py recording.wav [more.wav ...]
"""
import sys
from collections import Counter
import wave
from array import array


def tri(ph):
    q = ph if ph < 50 else 100 - ph
    return q * 1000 - 25000, abs(50 - ph) * 1000 - 25000


TRI = [tri(p) for p in range(100)]


def stereo(fr):
    while fr and fr[-1] == (0, 0):
        fr.pop()
    start = 0
    while start < len(fr) and fr[start] == (0, 0):
        start += 1
    i, checked, unknown, breaks, ph = start, 0, 0, [], None
    while i < len(fr):
        if ph is None:
            cand = [p for p in range(100) if TRI[p] == fr[i] and i + 1 < len(fr) and TRI[(p + 1) % 100] == fr[i + 1]]
            if not cand:
                unknown += 1
                i += 1
                continue
            ph = cand[0]
        if TRI[ph] != fr[i]:
            breaks.append(i)
            ph = None
            continue
        checked += 1
        ph = (ph + 1) % 100
        i += 1
    return checked >= 32 and not breaks and not unknown, f'{checked} matched frames, {len(breaks)} breaks, {unknown} unrecognised frames' + (f', first break at frame {breaks[0]}' if breaks else '')


def counter_index(fr):
    channels = len(fr[0])
    if channels != 6:
        return {2: 0, 5: 2}[channels]
    # Support current 3/4 and archived 5/6 diagnostics, including Consolidate gain.
    def score(ci):
        candidates = [f[ci + 1] for f in fr if 22500 < f[ci + 1] < 23500]
        if not candidates:
            return 0
        gain = Counter(candidates).most_common(1)[0][0] / 23130.0
        return sum((round(f[ci] / gain) & 7) == 0 and round(f[ci + 1] / gain) == 23130 for f in fr)
    return max((4, 2), key=score)


def stems(fr):
    if not fr:
        return False, 'empty recording'
    counter = counter_index(fr)
    fr = [(f[counter], f[counter + 1]) for f in fr]
    while fr and not any(fr[-1]):
        fr.pop()
    start = next((i for i in range(len(fr) - 1) if fr[i][1] & 0xFFFF == 0x5A5A and
                  fr[i][0] & 7 == 0 and (fr[i + 1][0] - fr[i][0]) & 0xFFFF == 1), None)
    if start is None:
        return False, 'no diagnostic frame counters found in the diagnostic channel pair'
    first, last, breaks = {}, {}, []
    previous = None
    for i in range(start, len(fr)):
        count, value = fr[i][0] & 0xFFFF, fr[i][1] & 0xFFFF
        lane = count & 7
        if (previous is not None and (count - previous) & 0xFFFF != 1) or (lane == 0 and value != 0x5A5A):
            breaks.append(i)
        previous = count
        if lane:
            first.setdefault(lane, value)
            last[lane] = value
    delta = {name: (last.get(lane, 0) - first.get(lane, 0)) & 0xFFFF for lane, name in
             ((1, 'underruns'), (2, 'overruns'), (3, 'missed packets'))}
    checked = len(fr) - start
    return checked >= 32 and not breaks and not any(delta.values()), f'{checked} counter frames, {len(breaks)} discontinuities, counter increases: {delta}'


def check(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if width not in (2, 3) or ch not in (2, 5, 6) or rate not in (44100, 48000) or (ch > 2 and rate != 44100):
        print(f'{path}: need 16/24-bit stereo at 44.1/48 kHz, or 5/6 channels at 44.1 kHz')
        return False
    if width == 2:
        values = array('h', raw)
        if sys.byteorder == 'big':
            values.byteswap()
    else:
        values = [int.from_bytes(raw[i:i + 3], 'little', signed=True) / 256.0 for i in range(0, len(raw), 3)]
    fr = [tuple(values[i:i + ch]) for i in range(0, len(values), ch)]
    if not fr:
        print(f'{path}: empty recording: FAIL')
        return False
    ci = counter_index(fr)
    candidates = [f[ci + 1] for f in fr if 22500 < f[ci + 1] < 23500]
    gain, counters = 1.0, ch > 2
    if len(candidates) > len(fr) / 64:
        trial = Counter(candidates).most_common(1)[0][0] / 23130.0
        pair = [(round(f[ci] / trial), round(f[ci + 1] / trial)) for f in fr]
        markers = sum((a & 7) == 0 and b == 23130 for a, b in pair)
        if markers > len(fr) / 64:
            gain, counters = trial, True
    if ch == 2 and not counters:
        peak = max(abs(v) for f in fr for v in f)
        if peak:
            gain = peak / 25000.0
    # Recover integer PCM after a constant Consolidate gain; reject resampling or
    # nonlinear processing rather than rounding arbitrary audio into a false pass.
    residual = max(abs(f[k] / gain - round(f[k] / gain)) for f in fr for k in (ci, ci + 1))
    if residual > 0.02:
        print(f'{path}: PCM altered beyond constant gain (residual {residual:.4f}): FAIL')
        return False
    fr = [tuple(round(v / gain) for v in f) for f in fr]
    ok, message = stems(fr) if counters else stereo(fr)
    print(f'{path}: {rate} Hz, {ch} channels, gain {gain:.8f}: {message}: {"PASS" if ok else "FAIL"}')
    return ok


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(0 if all([check(p) for p in sys.argv[1:]]) else 1)
