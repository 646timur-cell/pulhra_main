#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Instructions per sample of the audio path (mix_block) with the sounds given held, under valgrind (callgrind): two runs
# of 400 and 2400 blocks, the difference over 2000 x 32 samples. Host x86-64 at -O2: a comparison between builds and
# sounds (as tests/regress.c's CPU check), not the FM-1's cycles.
#   tests/strange_cpu.sh STRANGE(0|1) T:ENGINE:PRESET:NOTES ...
set -e
s=$1; shift
mkdir -p build/host
cc -O2 -w -DFELUCCA_STRANGE=$s -Ibuild/gen -Ifirmware/src -Itests -o build/host/strange_bench_$s tests/strange_bench.c -lm
ir() { valgrind --tool=callgrind --callgrind-out-file=/dev/null build/host/strange_bench_$s "$@" 2>&1 | sed -n 's/.*Collected : \([0-9]*\).*/\1/p'; }
a=$(ir 400 "$@"); b=$(ir 2400 "$@")
echo "$(( (b - a) / 64000 )) instructions/sample  ($*)"
