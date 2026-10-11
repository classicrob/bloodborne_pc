#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-entry-trace
clang -arch x86_64 -O2 -g -std=c11 -Wall -Wextra -Werror \
    tests/test_guest_entry_mac.c tests/test_guest_entry_mac.S -o out/mac-entry-trace/test
python3 packaging/macos/sign.py out/mac-entry-trace/test
arch -x86_64 out/mac-entry-trace/test >out/mac-entry-trace/stdout.log 2>out/mac-entry-trace/stderr.log
python3 - <<'PY'
from pathlib import Path
import re
p=Path('out/mac-entry-trace')
result=(p/'stdout.log').read_text()
lines=[s for s in (p/'stderr.log').read_text().splitlines() if s.startswith('GUEST-ENTRY seq=')]
assert len(lines)==34, len(lines)
assert {int(re.search(r'seq=0x([0-9a-f]+)',s)[1],16) for s in lines}==set(range(1,35))
assert {int(re.search(r'thread=0x([0-9a-f]+)',s)[1],16) for s in lines}=={0,42,100,101,102,103}
caller=re.search(r'caller=([0-9a-f]+)',result)[1]
assert all('caller=0x'+caller in s and 'rsi-words[unreadable-mach=' in s for s in lines)
print(result,end='')
PY
