#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read PULHRA audio counters; never write flash or change transport.
Requires mido + python-rtmidi. Optional --capture runs the reviewed CoreAudio
capture helper: --capture /tmp/capture-fm1 --output FILE.wav --seconds 20.
Without --capture, samples the counters around a short wait.
"""
import argparse
import json
import subprocess
import time
from pathlib import Path

FIELDS = ('ticks halves late max_us last_us cpu_q8 timer_irqs packets frames '
          'ring_write ring_read fill underruns overruns missed stalls adj_up '
          'adj_down starts half_frames ticks_per_us').split()


def decode(data):
    if len(data) < 2 or data[0] != 1 or data[1] != len(FIELDS) or len(data) != 2 + 5 * len(FIELDS):
        raise ValueError('unsupported diagnostic schema or length')
    result = {}
    for i, name in enumerate(FIELDS):
        chunk = data[2 + 5 * i:7 + 5 * i]
        if chunk[-1] > 15 or any(v > 127 for v in chunk):
            raise ValueError('invalid 32-bit diagnostic field')
        result[name] = sum(v << (7 * j) for j, v in enumerate(chunk))
    return result


def rates(before, after, elapsed):
    delta = {k: (after[k] - before[k]) & 0xFFFFFFFF for k in FIELDS}
    device_seconds = delta['ticks'] / (after['ticks_per_us'] * 1e6)
    if device_seconds <= 0:
        raise ValueError('non-positive device interval')
    return {'host_seconds': elapsed, 'device_seconds': device_seconds,
            'device_ticks_per_host_second': delta['ticks'] / elapsed,
            'processed_audio_frames_per_device_second': delta['halves'] * after['half_frames'] / device_seconds,
            'queued_usb_frames_per_device_second': delta['frames'] / device_seconds,
            'usb_packets_per_device_second': delta['packets'] / device_seconds,
            'ring_written_frames_per_device_second': delta['ring_write'] / device_seconds,
            'timer_irqs_per_device_second': delta['timer_irqs'] / device_seconds,
            'late_delta': delta['late'], 'underrun_delta': delta['underruns'],
            'overrun_delta': delta['overruns'], 'missed_delta': delta['missed'],
            'stalls_delta': delta['stalls'], 'starts_delta': delta['starts']}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port')
    ap.add_argument('--seconds', type=int, default=5)
    ap.add_argument('--capture', type=Path)
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    if not 1 <= args.seconds <= 30:
        ap.error('--seconds must be 1..30')
    if args.capture and (not args.output or args.output.exists()):
        ap.error('--capture requires a new --output path (existing files are not overwritten)')
    import mido
    name = args.port or next((p for p in mido.get_output_names() if 'FM-1' in p), None)
    if not name:
        ap.error('no FM-1 MIDI port; specify --port')
    with mido.open_input(name) as inp, mido.open_output(name) as out:
        def request(cmd):
            start = time.monotonic()
            out.send(mido.Message('sysex', data=[0x7D, 0x50, 0x48, cmd]))
            while time.monotonic() - start < 3:
                for msg in inp.iter_pending():
                    if msg.type == 'sysex' and list(msg.data[:4]) == [0x7D, 0x50, 0x48, cmd]:
                        data = list(msg.data[4:])
                        if not data or data[0]:
                            raise RuntimeError(f'command {cmd} unavailable/status {data[:1]}')
                        return data[1:], (start + time.monotonic()) / 2
                time.sleep(.005)
            raise TimeoutError(f'no reply to read-only command {cmd}')
        info, _ = request(1)
        version = bytes(info[2 + 7 * info[1]:]).split(b'\0')[0].decode('ascii')
        first, t0 = request(7)
        before = decode(first)
        if args.capture:
            subprocess.run([str(args.capture), str(args.output), str(args.seconds)], check=True)
        else:
            time.sleep(args.seconds)
        last, t1 = request(7)
        after = decode(last)
    report = {'firmware': version, 'before': before, 'after': after, 'rates': rates(before, after, t1 - t0)}
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.with_suffix('.stats.json').write_text(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
