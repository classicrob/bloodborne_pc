#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/toggle-file
# build <output> <compiler and flags...>: native elsewhere, signed x86_64 under Rosetta on Darwin.
build() {
    out=$1; shift
    if [ "$(uname -s)" = Darwin ]; then
        "$@" -arch x86_64 -o "$out"
        python3 packaging/macos/sign.py "$out"
    else
        "$@" -pthread -o "$out"
    fi
}
run() { if [ "$(uname -s)" = Darwin ]; then arch -x86_64 "$@"; else "$@"; fi; }
build out/toggle-file/test "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc tests/test_toggle_file.c
run out/toggle-file/test
# BbToggle::Disabled reads word 2 for Word2 bits, word 1 for everything else.
build out/toggle-file/helpers "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I. -x c++ - <<'CPP'
#include "gpu/shim/bbport_toggles.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
extern "C" {
std::uint64_t runtime_disabled_optimizations, runtime_disabled_optimizations2;
__thread sigjmp_buf* runtime_fault_recover;
}
static_assert(!std::is_convertible_v<BbToggle::Word2, std::uint64_t>);
int main() {
    using BbToggle::Word2;
    runtime_disabled_optimizations2 = 1 | 4;
    assert(BbToggle::Disabled(Word2(1)) && !BbToggle::Disabled(Word2(2)));
    assert(BbToggle::Disabled(Word2(2) | Word2(4)));
    assert(!BbToggle::Disabled(1) && !BbToggle::Disabled(BbToggle::RegionCache));
    runtime_disabled_optimizations = BbToggle::RegionCache;
    runtime_disabled_optimizations2 = 0;
    assert(BbToggle::Disabled(BbToggle::RegionCache) && !BbToggle::Disabled(Word2(1)));
    std::puts("PASS word 2 helpers");
}
CPP
run out/toggle-file/helpers
