# SPDX-License-Identifier: GPL-3.0-only
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from uac_tone_check import stereo, stems, TRI


class RecordingTest(unittest.TestCase):
    def test_stereo(self):
        good = [TRI[i % 100] for i in range(500)]
        self.assertTrue(stereo(good.copy())[0])
        self.assertTrue(stereo([(0, 0)] * 50 + good + [(0, 0)] * 50)[0])
        for bad in ([(0, 0)] * 100, [(123, 456)] * 100, good[:200] + good[201:], good[:200] + good[199:], good[:200] + [(123, 456)] + good[201:]):
            self.assertFalse(stereo(bad.copy())[0])

    def test_stems(self):
        def frames(overrun=False):
            result = []
            for i in range(65504, 65904):  # includes the 16-bit counter wrap
                value = 0x5A5A if i & 7 == 0 else (1 if overrun and i > 65700 and i & 7 == 2 else 0)
                result.append((123, 456, 789, 1234, i & 0xFFFF, value))
            return result
        good = frames()
        self.assertTrue(stems(good.copy())[0])
        self.assertFalse(stems(good[:100] + good[101:])[0])
        self.assertFalse(stems(good[:100] + good[99:])[0])
        self.assertFalse(stems(frames(True))[0])
        self.assertFalse(stems([(1, 2, 3, 4, 5, 6)] * 100)[0])


if __name__ == '__main__':
    unittest.main()
