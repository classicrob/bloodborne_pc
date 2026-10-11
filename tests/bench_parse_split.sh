#!/bin/bash
# Builds and runs tests/bench_parse_split.cpp (x86_64; Rosetta on Apple Silicon), -O2, signed.
set -e
cd "$(dirname "$0")/.."
mkdir -p out/bench
clang++ -arch x86_64 -std=c++20 -O2 -Igpu/shim tests/bench_parse_split.cpp -o out/bench/parse_split
codesign --force --sign - out/bench/parse_split
arch -x86_64 out/bench/parse_split
