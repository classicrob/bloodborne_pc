#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/cpu-profile
capture_dir=$(mktemp -d "$PWD/out/cpu-profile/run.XXXXXX")
clang -arch x86_64 -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -pthread \
    tests/test_cpu_profile_macos.c src/compat_mac.c -I. -Isrc -Wl,-dead_strip -o "$capture_dir/test"
python3 packaging/macos/sign.py "$capture_dir/test"
arch -x86_64 "$capture_dir/test" off
BB_CPU_PROFILE="$capture_dir/safety.jsonl" arch -x86_64 "$capture_dir/test"
BB_CPU_PROFILE="$capture_dir/window.jsonl" BB_CPU_PROFILE_WINDOW=0.05:1.5 arch -x86_64 "$capture_dir/test" window
BB_CPU_PROFILE="$capture_dir/overflow.jsonl" arch -x86_64 "$capture_dir/test" overflow
clang -arch x86_64 -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -pthread \
    -DBB_TEST_CPU_PROFILE tests/test_guest_entry_mac.c tests/test_guest_entry_mac.S -o "$capture_dir/entry-test"
python3 packaging/macos/sign.py "$capture_dir/entry-test"
BB_CPU_PROFILE="$capture_dir/entry.jsonl" arch -x86_64 "$capture_dir/entry-test" >"$capture_dir/entry.log" 2>&1
python3 tools/macos/cpu_profile.py "$capture_dir/safety.jsonl" --folded "$capture_dir/stacks.folded" >"$capture_dir/report.txt" 2>"$capture_dir/symbols.log"
python3 - "$capture_dir" <<'PY'
from pathlib import Path
from collections import Counter
import json, sys
p=Path(sys.argv[1])
for name in ('safety','window','entry','overflow'):
    rows=[json.loads(s) for s in (p/f'{name}.jsonl').read_text().splitlines()]
    summary=next(r for r in rows if r['type']=='summary')
    assert (summary['ring_dropped']>=100 if name=='overflow' else not summary['ring_dropped']), summary
    assert not summary['thread_table_full'] and not summary['image_table_full'], summary
    samples=[r for r in rows if r['type']=='sample']
    assert len(samples)==summary['samples'] and samples
    if name=='safety':
        counts=Counter(s['name'] for s in samples)
        assert counts['hot-alpha']>=10 and counts['hot-beta']>=10, counts
        assert summary['read_faults']==3, summary
        assert any(s['flags']&1 and len(s['pc'])==1 for s in samples), 'uncached thread must be RIP-only'
    if name=='window':
        cfg=next(r for r in rows if r['type']=='config')
        elapsed=[(s['ticks']-cfg['start_ticks'])*cfg['numer']/cfg['denom']/1e9 for s in samples]
        assert min(elapsed)>=0.05 and max(elapsed)<1.8, (min(elapsed), max(elapsed))
report=(p/'report.txt').read_text()
assert 'hot_alpha' in report and 'hot_beta' in report, report
assert (p/'stacks.folded').read_text()
assert '34 concurrent hits' in (p/'entry.log').read_text()
print('CPU profile: windows, offline symbols/folded stacks and SIGTRAP/all-YMM coexistence PASS')
print('Evidence:',p)
PY
