#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""One self-contained web installer page for the sound-module build: Felucca's installer (index_pkg.html,
fm1pkg.js, fm1ota.js, fm1backup.js inlined) with the package itself inside the page as a data: URL, so
the page needs no server: open it in Chrome or Edge (a file on disk, a GitHub Pages site, anywhere).

  web/make_module_installer.py build/felucca-1.5.1-mod.fwsc 1.5.1-mod OUT.html
"""
import base64
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from make_site import product_of, strip_module  # noqa: E402

STRANGE_TITLE = "FM-1 STRANGE"
STRANGE_CREDIT = ('<p class="small">STRANGE build: two MIDI parts, the engines LYRA (organismic pairs, hyper-LFO, delay), '
                  'CHAOS (unstable oscillators), STRING, MODAL and BLOW (waveguides and resonators), with PHYS and GURDY. '
                  'A sound-module build of Felucca, below.</p>\n')
MONOFM_TITLE = "FM-1 MONOFM"
MONOFM_CREDIT = ('<p class="small">MONOFM build: the four-track groovebox of Felucca (64 steps, p-locks, REC, the FX layer) '
                 'playing only the darkmachine engines: STRING, MODAL, BLOW, RESO, JUNK, LYRA, CHAOS, DRIFT, the slice '
                 'machines and PLANT. Built on Felucca, below.</p>\n')
SWAMP_TITLE = "FM-1 SWAMP MACHINE"
SWAMP_CREDIT = ('<p class="small">SWAMP MACHINE build: its own four-track step sequencer app on the MONOFM sound core '
                '(the darkmachine engines). Built on Felucca, below.</p>\n')
TITLE = "FM-1 Sound Module"
CREDIT = ('<p class="small">Sound-module build: TRACK CH MIDI, per-track transpose, HOME levels / TRANSPOSE, no '
          'sequencer; the GURDY engine from <a href="https://github.com/jasonpersinger/ghoulbox-fm1-dungeon-synth" '
          'target="_blank" rel="noopener" style="color:inherit">GHOULBOX</a> (Jason Persinger, GPL-3.0). Built on '
          'Felucca, below.</p>\n')


def main(pkg, version, out):
    global TITLE, CREDIT
    if "strange" in version:                            # the STRANGE build (STRANGE.md): its own title and line
        TITLE, CREDIT = STRANGE_TITLE, STRANGE_CREDIT
    elif "monofm" in version:                           # the MONOFM build (MONOFM.md)
        TITLE, CREDIT = MONOFM_TITLE, MONOFM_CREDIT
    elif "swamp" in version:                            # SWAMP MACHINE (firmware/src/swamp.c)
        TITLE, CREDIT = SWAMP_TITLE, SWAMP_CREDIT
    raw = Path(pkg).read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_9\d\d", product):
        raise SystemExit(f"{pkg}: identity {product!r} is not a Felucca package (FM-1_9xx)")
    if b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: no Felucca loader in it")
    html = (HERE / "index_pkg.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((HERE / f).read_text(encoding="utf-8"))
                    for f in ("fm1pkg.js", "fm1ota.js", "fm1backup.js"))
    data = "data:application/octet-stream;base64," + base64.b64encode(raw).decode()
    meta = json.dumps({"version": version, "product": product, "pkg": data})
    for mark in ("/*LIB*/", "/*META*/", "<title>Felucca</title>", "<h1>Felucca</h1>"):
        if html.count(mark) != 1:
            raise SystemExit(f"index_pkg.html must contain {mark} once; update make_module_installer.py")
    html = html.replace("/*LIB*/", lib).replace("/*META*/", meta)
    html = html.replace("<title>Felucca</title>", f"<title>{TITLE}</title>")
    html = html.replace("<h1>Felucca</h1>", f"<h1>{TITLE}</h1>\n  {CREDIT}")
    html = html.replace('<p><a href="../editor/" style="color:inherit" data-t="editor"></a></p>',
                        '<p><a href="https://hugelton.github.io/Felucca/webapp/editor/" target="_blank" rel="noopener" '
                        'style="color:inherit" data-t="editor"></a></p>')
    html = re.sub(r'href="\.\./\.\./firmware/[^"]*"', 'href="https://github.com/hugelton/Felucca/tree/main/LICENSES"', html)
    Path(out).write_text(html, encoding="utf-8")
    print(f"installer: {out} ({len(html)} B), package {len(raw)} B, {product}, version {version}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
