#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Resolve a GitHub build into a channel; never infer stable from a branch."""
import json
import os
import re
import subprocess
from pathlib import Path

TAG = re.compile(r"v([0-9]\.[0-9]\.[0-9])(-rc[1-9][0-9]*)?")


def resolve(ref, sha, source_version):
    if ref.startswith('refs/tags/'):
        match = TAG.fullmatch(ref.removeprefix('refs/tags/'))
        if not match:
            raise ValueError('Release tag must be vX.Y.Z or vX.Y.Z-rcN (one digit per component, as supported by build.py)')
        return {'channel': 'rc' if match[2] else 'stable', 'version': ''.join(match.groups(default='')), 'sha': sha}
    channel = 'preview' if ref == 'refs/heads/develop' else 'ci'
    return {'channel': channel, 'version': f'{source_version}-dev{sha[:7]}', 'sha': sha}


def main():
    source = Path('firmware/src/pulhra.c').read_text()
    version = re.search(r'#define FELUCCA_VERSION "v([^"]+)"', source)[1]
    info = resolve(os.environ['GITHUB_REF'], os.environ['GITHUB_SHA'], version)
    if info['channel'] == 'stable':
        subprocess.run(['git', 'merge-base', '--is-ancestor', info['sha'], 'origin/main'], check=True)
    Path('build').mkdir(exist_ok=True)
    Path('build/channel.json').write_text(json.dumps(info, indent=2) + '\n')
    with open(os.environ['GITHUB_OUTPUT'], 'a') as out:
        for key, value in info.items():
            out.write(f'{key}={value}\n')


if __name__ == '__main__':
    main()
