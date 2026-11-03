# SPDX-License-Identifier: GPL-3.0-only
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from fm1_audio_stats import FIELDS, decode, rates


class StatsTest(unittest.TestCase):
    def test_unsigned_fields_and_schema(self):
        values = [0xFFFFFFFF - i for i in range(len(FIELDS))]
        payload = [1, len(FIELDS)] + [v >> (7 * j) & 127 for v in values for j in range(5)]
        self.assertEqual(list(decode(payload).values()), values)
        with self.assertRaises(ValueError):
            decode(payload[:-1])
        payload[-1] = 16
        with self.assertRaises(ValueError):
            decode(payload)

    def test_wrap_and_rates(self):
        a = dict.fromkeys(FIELDS, 0); b = a.copy()
        a['ticks'] = 0xFFFFFF00
        b.update(ticks=(a['ticks'] + 24000000) & 0xFFFFFFFF, ticks_per_us=24, half_frames=128, halves=344, packets=1000, frames=44100, ring_write=44032, timer_irqs=10000)
        r = rates(a, b, 1)
        self.assertEqual(r['device_seconds'], 1)
        self.assertEqual(r['processed_audio_frames_per_device_second'], 44032)
        self.assertEqual(r['usb_packets_per_device_second'], 1000)
        self.assertEqual(r['device_ticks_per_host_second'], 24000000)


if __name__ == '__main__':
    unittest.main()
