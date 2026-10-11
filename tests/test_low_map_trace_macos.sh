#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-low-trace
clang -arch x86_64 -O2 -g -std=c11 -Wall -Wextra -Werror -Isrc \
    tests/test_low_map_trace_macos.c src/runtime_memory.c -o out/mac-low-trace/test
python3 packaging/macos/sign.py out/mac-low-trace/test
BB_LOW_MAP_TRACE=0 arch -x86_64 out/mac-low-trace/test 2>out/mac-low-trace/off.log
BB_LOW_MAP_TRACE=1 arch -x86_64 out/mac-low-trace/test 2>out/mac-low-trace/on.log
python3 - <<'PY'
from pathlib import Path
import re
root=Path('out/mac-low-trace')
assert 'LOW-MEM' not in (root/'off.log').read_text()
lines=[line for line in (root/'on.log').read_text().splitlines() if line.startswith('LOW-MEM')]
assert len(lines)==6, lines
assert [int(re.search(r'#(\d+)',s)[1]) for s in lines]==list(range(1,7))
assert 'map owner=guest-stack' in lines[0]
assert 'map owner=host-heap' in lines[1]
assert [s.split()[2] for s in lines[2:]]==['alloc','free','alloc','free']
assert len({re.search(r'\[(0x[0-9a-f]+),',s)[1] for s in lines[2:]})==1
assert all('caller=0x' in s and 'image=' in s for s in lines)
print('PASS: low-map owner/range/caller, suballocation free/reuse, disabled trace')
PY
