# SPDX-License-Identifier: GPL-3.0-only
"""Release routing must protect stable files and reject accidental downgrades."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from release_channel import resolve
from publish_site import publish


class ReleaseTest(unittest.TestCase):
    def test_channels(self):
        for ref, channel, version in [
            ('refs/heads/main', 'ci', '1.5.1-devabcdef0'),
            ('refs/heads/feature/fx', 'ci', '1.5.1-devabcdef0'),
            ('refs/heads/develop', 'preview', '1.5.1-devabcdef0'),
            ('refs/tags/v1.5.2', 'stable', '1.5.2'),
            ('refs/tags/v1.5.2-rc1', 'rc', '1.5.2-rc1'),
        ]:
            with self.subTest(ref=ref):
                result = resolve(ref, 'abcdef012345', '1.5.1')
                self.assertEqual((result['channel'], result['version']), (channel, version))
        for tag in ('vlatest', 'v1.5', 'v1.5.2-beta', 'v1.5.2-rc0', 'v1.5.20'):
            with self.assertRaises(ValueError):
                resolve('refs/tags/' + tag, 'abcdef0', '1.5.1')

    def test_channel_preservation_and_downgrade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, dest = root / 'source', root / 'published'
            source.mkdir()
            def build(channel, version, sha, body):
                (source / 'CHANNEL.json').write_text(json.dumps(dict(channel=channel, version=version, sha=sha)))
                (source / 'index.html').write_text(body)
                publish(source, dest)
            build('stable', '1.5.2', 'a', 'stable')
            build('preview', '1.5.3-devb', 'b', 'preview')
            self.assertEqual((dest / 'index.html').read_text(), 'stable')
            build('stable', '1.5.3', 'c', 'new stable')
            self.assertEqual((dest / 'preview/index.html').read_text(), 'preview')
            for channel, version, sha in [('stable', '1.5.2', 'a'), ('stable', '1.5.3', 'd'), ('rc', '1.5.4-rc1', 'e')]:
                with self.assertRaises(ValueError):
                    build(channel, version, sha, 'bad')
            self.assertEqual((dest / 'index.html').read_text(), 'new stable')


if __name__ == '__main__':
    unittest.main()
