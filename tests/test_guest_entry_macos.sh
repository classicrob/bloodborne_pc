#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-entry-trace
clang -arch x86_64 -O2 -g -std=c11 -Wall -Wextra -Werror \
    tests/test_guest_entry_mac.c tests/test_guest_entry_mac.S -o out/mac-entry-trace/test
python3 packaging/macos/sign.py out/mac-entry-trace/test
for mode in immediate delayed; do
    if [ "$mode" = delayed ]; then
        arch -x86_64 out/mac-entry-trace/test delayed >out/mac-entry-trace/stdout.log 2>out/mac-entry-trace/stderr.log
    else
        arch -x86_64 out/mac-entry-trace/test >out/mac-entry-trace/stdout.log 2>out/mac-entry-trace/stderr.log
    fi
python3 - "$mode" <<'PY'
from pathlib import Path
import re, sys
p=Path('out/mac-entry-trace')
result=(p/'stdout.log').read_text()
all_lines=(p/'stderr.log').read_text().splitlines()
arms=[i for i,s in enumerate(all_lines) if s=='GUEST-ENTRY armed before sceAvPlayerStart']
assert len(arms)==(1 if sys.argv[1]=='delayed' else 0)
if arms:
    assert not any(s.startswith('GUEST-ENTRY seq=') for s in all_lines[:arms[0]])
lines=[s for s in all_lines if s.startswith('GUEST-ENTRY seq=')]
assert len(lines)==34, len(lines)
assert {int(re.search(r'seq=0x([0-9a-f]+)',s)[1],16) for s in lines}==set(range(1,35))
assert {int(re.search(r'thread=0x([0-9a-f]+)',s)[1],16) for s in lines}=={0,42,100,101,102,103}
caller=re.search(r'caller=([0-9a-f]+)',result)[1]
assert all('caller=0x'+caller in s and 'rsi-words[unreadable-mach=' in s for s in lines)
assert not any('name-unreadable' in s or 'thread-port-release' in s for s in lines)
identities=re.findall(r'IDENTITY thread=(\d+) pthread=([0-9a-f]+) name=(.*)',result)
assert len(identities)==10
for thread, host, name in identities:
    matching=[s for s in lines if f'thread=0x{int(thread):016x} ' in s and
              f'pthread=0x{host} ' in s and f'name="{name}" ' in s]
    assert len(matching)==(1 if int(thread)<100 else 4), (thread,host,name,matching)
print(result,end='')
PY
    cp out/mac-entry-trace/stdout.log "out/mac-entry-trace/$mode-stdout.log"
    cp out/mac-entry-trace/stderr.log "out/mac-entry-trace/$mode-stderr.log"
done
