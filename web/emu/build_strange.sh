#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The STRANGE build in the browser without Emscripten: the same felucca_web.c compiled by zig cc (its bundled WASI libc,
# pip install ziglang) to a wasm with no imports, which web/emu/worklet.js loads as it loads Emscripten's.
#   web/emu/build_strange.sh  ->  build/emu_s/{index.html, worklet.js, felucca.wasm, fonts/}
set -e
cd "$(dirname "$0")/../.."
[ -f build/gen/felucca_tables.h ] || { echo "emu: run the generator once first (build/gen)"; exit 1; }
V=1.5.1-strange
mkdir -p build/emu_s/fonts
cat > build/emu_s/shim.h <<'H'
/* WASI has no processes: hostsim.c's fork / pipe / wait paths (the regression runner only) never run here */
#define fork() (-1)
#define pipe(f) (-1)
#define _SYS_WAIT_H
#define waitpid(p, s, o) (-1)
#define wait(s) (-1)
#define WIFEXITED(s) 0
#define WEXITSTATUS(s) 0
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0
H
python3 -m ziglang cc -target wasm32-wasi -O2 -ffp-contract=off -std=gnu99 -w -D_WASI_EMULATED_PROCESS_CLOCKS \
    -D_WASI_EMULATED_SIGNAL -include build/emu_s/shim.h -DFELUCCA_STRANGE=1 "-DFELUCCA_VERSION=\"$V\"" \
    -Ibuild/gen -Ifirmware/src -mexec-model=reactor -Wl,--export-dynamic -Wl,-z,stack-size=1048576 \
    -Wl,--initial-memory=33554432 -lwasi-emulated-process-clocks -lwasi-emulated-signal \
    -o build/emu_s/felucca.wasm web/emu/felucca_web.c
sed -e "s/@VERSION@/$V/g" -e "s/@PREVIEW@/ (STRANGE)/g" -e 's/<title>Felucca<\/title>/<title>FM-1 STRANGE<\/title>/' \
    -e 's/felucca-emu-flash/fm1-strange-emu-flash/' web/emu/index.html > build/emu_s/index.html
cp web/emu/worklet.js build/emu_s/
cp web/emu/fonts/DotGothic16-subset.woff web/emu/fonts/OFL.txt build/emu_s/fonts/
echo "emu: build/emu_s ($(wc -c < build/emu_s/felucca.wasm | tr -d ' ') B wasm)"
