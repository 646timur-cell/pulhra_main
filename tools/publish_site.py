#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Copy one channel into a checked-out Pages branch, preserving the other channel."""
import json
import shutil
import sys
from pathlib import Path


def publish(source, destination):
    source, destination = Path(source), Path(destination)
    info = json.loads((source / 'CHANNEL.json').read_text())
    if info['channel'] not in ('stable', 'preview'):
        raise ValueError('Only stable and preview may be published to Pages')
    target = destination / 'preview' if info['channel'] == 'preview' else destination
    if info['channel'] == 'stable' and (target / 'CHANNEL.json').exists():
        old = json.loads((target / 'CHANNEL.json').read_text())
        order = lambda v: tuple(map(int, v.split('.')))
        if order(info['version']) < order(old['version']):
            raise ValueError('Refusing to replace stable with an older version')
        if info['version'] == old['version'] and info['sha'] != old['sha']:
            raise ValueError('A stable version cannot refer to a different commit')
    target.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.name.startswith('.') or entry.name == 'preview':
            raise ValueError(f'Unexpected site entry: {entry.name}')
        if entry.is_dir():
            shutil.copytree(entry, target / entry.name, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, target / entry.name)
    (destination / '.nojekyll').touch()


if __name__ == '__main__':
    publish(*sys.argv[1:])
