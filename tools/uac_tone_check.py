#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check unprocessed 16-bit WAV recordings of PULHRA USB diagnostic builds.
Stereo: exact diagnostic triangles at 44.1/48 kHz. Six channels: DRUMS
counters in channels 5/6, 44.1 kHz. Exit 1 for corruption or unrecognised audio.
  python3 tools/uac_tone_check.py recording.wav [more.wav ...]
"""
import sys
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


def stems(fr):
    while fr and not any(fr[-1]):
        fr.pop()
    start = next((i for i in range(len(fr) - 1) if fr[i][5] & 0xFFFF == 0x5A5A and
                  fr[i][4] & 7 == 0 and (fr[i + 1][4] - fr[i][4]) & 0xFFFF == 1), None)
    if start is None:
        return False, 'no diagnostic frame counters found in channels 5/6'
    first, last, breaks = {}, {}, []
    previous = None
    for i in range(start, len(fr)):
        count, value = fr[i][4] & 0xFFFF, fr[i][5] & 0xFFFF
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
    if width != 2 or ch not in (2, 6) or rate not in (44100, 48000) or (ch == 6 and rate != 44100):
        print(f'{path}: need 16-bit stereo at 44.1/48 kHz or 6 channels at 44.1 kHz')
        return False
    a = array('h', raw)
    if sys.byteorder == 'big':
        a.byteswap()
    fr = [tuple(a[i:i + ch]) for i in range(0, len(a), ch)]
    ok, message = stereo(fr) if ch == 2 else stems(fr)
    print(f'{path}: {rate} Hz, {ch} channels: {message}: {"PASS" if ok else "FAIL"}')
    return ok


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(0 if all([check(p) for p in sys.argv[1:]]) else 1)
