#!/bin/sh
# The parse split's constant-ring line (tests/test_ring_counters.cpp): baselined at the first frame
# end, then per-frame counts of exactly the activity inside the window.
set -eu
cd "$(dirname "$0")/.."
out=out/ring-counters-test
mkdir -p "$out"
"${CXX:-clang++}" -arch x86_64 -std=c++20 -O1 -Wall -Wextra -Werror -Igpu/shim \
    tests/test_ring_counters.cpp -o "$out/driver"
codesign --force --sign - "$out/driver" 2>/dev/null
"$out/driver" > "$out/block.txt"
line=$(grep "Constant ring per frame" "$out/block.txt") || { echo "FAIL: no constant ring line"; cat "$out/block.txt"; exit 1; }
want="  Constant ring per frame: 1 copied at stage A (1 KiB; buffer resources and flat user data), 0.1 not copied (ring full); buffer resources skipped to ObtainBuffer: large 0.1, GPU-modified 0.0, pending write 0.2 (behind a merged pending range >= 64 MiB 0.1); pending-write notes 0 (>= 64 MiB 0.1, of which written V#s 0.1)"
if [ "$line" != "$want" ]; then
    echo "FAIL: constant ring line"; echo "  got:  $line"; echo "  want: $want"; exit 1
fi
echo "ring-counters-test: PASS"
