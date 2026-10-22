# PULHRA

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

A minimal Elektron-style groovebox firmware for the M-VAVE FM-1: four tracks, patterns in banks A–E,
parameter locks, trig states, chains, an arpeggiator per track and per-pattern kits, on the MONOFM sound core.

- **What it is and how to play it:** [PULHRA.md](PULHRA.md)
- **Install:** open `fm1-pulhra-installer.html` from the [`firmware-builds`](../../tree/firmware-builds) branch in Chrome
  with the FM-1 on USB. `SOURCE_COMMIT` in that branch names the commit it was built from.
- **Build:** [BUILDING.md](BUILDING.md). Host tests:
  `cc -O2 -w -DFELUCCA_SWAMP=1 -DFELUCCA_MONOFM=1 -DFELUCCA_MODULE=0 -Ibuild/gen -Ifirmware/src -o build/host/pulhra_test tests/pulhra_test.c -lm`
- **Design and plans:** `docs/superpowers/specs/`, `docs/superpowers/plans/`

## Where it comes from

PULHRA is a fork of MONOFM, which is built on [Felucca](https://github.com/hugelton/Felucca) by hugelton
(GPL-3.0-only). Felucca's own README is kept as [FELUCCA.md](FELUCCA.md); licensing is in [LICENSING.md](LICENSING.md)
and [LICENSES/](LICENSES). The history before this repository lives in `fm1-sound-module` (branch `pulhra`).
