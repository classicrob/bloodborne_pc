#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-playability
clang -arch x86_64 -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
    tests/test_player_position_macos.c -o out/mac-playability/test-player-position
python3 packaging/macos/sign.py out/mac-playability/test-player-position
log=$(mktemp out/mac-playability/player-position.XXXXXX)
trap 'rm -f "$log"' EXIT
arch -x86_64 out/mac-playability/test-player-position "$@" > "$log"
cat "$log"
python3 - "$log" <<'PYJSON'
import json, math, sys
records=[json.loads(line) for line in open(sys.argv[1]) if line.startswith('{')]
assert len(records)==2, records
valid, invalid=records
assert valid['version']==1 and valid['valid'] is True and valid['reason']=='ok'
assert valid['player']=='0x2000' and valid['map_raw']==0x18010000
assert all(math.isclose(a,b,abs_tol=1e-4) for a,b in zip(valid['xyz'],[-193.4,-28.646,68.5]))
assert valid['player_yaw_rad']==1.25
assert valid['pid']>0 and valid['run_id']=='000000000000007b'
assert valid['sequence']==1 and invalid['sequence']==2
assert invalid['valid'] is False and invalid['reason']=='absent-player-chain'
assert not ({'xyz','player','player_yaw_rad','map_raw'} & invalid.keys())
for r in records:
    assert r['started_monotonic_ns']<=r['sample_monotonic_ns']<=r['published_monotonic_ns']
    assert r['sample_age_ns']==r['published_monotonic_ns']-r['sample_monotonic_ns']
    assert r['pad_reads']==0 and r['source']=='pad-read-observation'
print('PASS: parsed JSON schema, coordinates, identity, timestamps, sequence and invalid omission')
PYJSON
