#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Assemble the installer site and release assets from already built packages."""
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
build = root / 'build'
info = json.loads((build / 'channel.json').read_text())
version = info['version']
site = build / 'site'
site.mkdir(exist_ok=True)


def installer(package, name):
    subprocess.run(['python3', str(root / 'web/make_module_installer.py'), str(package), version, str(site / name)], check=True)


os.environ['PULHRA_CHANNEL'] = info['channel']
os.environ['PULHRA_STEM_CHANNELS'] = '6'
info['stem_channels'] = 6
installer(build / f'pulhra-{version}.fwsc', 'fm1-pulhra-installer.html')
shutil.copy2(site / 'fm1-pulhra-installer.html', site / 'index.html')
for source, name in [('pulhra_editor.html', 'pulhra-editor.html'), ('pulhra_keys.html', 'keys.html')]:
    shutil.copy2(root / 'web' / source, site / name)
for name in ('stereo-tone', 'stems', 'stems-tone'):
    package = build / f'pulhra-{version}-{name}.fwsc'
    if package.exists():
        installer(package, f'{name}.html')
        shutil.copy2(package, site / package.name)
shutil.copy2(build / f'pulhra-{version}.fwsc', site)
shutil.copytree(build / f'release-{version}', site / 'release', dirs_exist_ok=True)
(site / 'SOURCE_COMMIT').write_text(info['sha'] + '\n')
(site / 'CHANNEL.json').write_text(json.dumps(info, indent=2) + '\n')
(site / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in sorted(site.glob('*.fwsc'))))
