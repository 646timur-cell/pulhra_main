#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# MONOFM: instructions per sample of the whole audio path (mix_block) with four tracks playing a pattern (tests/monofm_test.c
# BENCH, SET 0..2), under valgrind: two runs of 1 s and 3 s, the difference over 2 s. Host x86-64 at -O2: compare with
# tests/strange_cpu.sh's figures (STRANGE.md), not the FM-1's cycles.
#   tests/monofm_cpu.sh [SET]
set -e
mkdir -p build/host
cc -O2 -w -Ibuild/gen -Ifirmware/src -Itests -o build/host/monofm_bench tests/monofm_test.c -lm
ir() { BENCH=$1 SET=${2:-0} valgrind --tool=callgrind --callgrind-out-file=/dev/null build/host/monofm_bench 2>&1 | sed -n 's/.*Collected : \([0-9]*\).*/\1/p'; }
a=$(ir 1 "$1"); b=$(ir 3 "$1")
echo "$(( (b - a) / 88200 )) instructions/sample  (SET ${1:-0})"
