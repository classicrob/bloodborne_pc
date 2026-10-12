#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/mac-playability
for mode in fake real; do
    extra=
    [ "$mode" != real ] || extra=-DBB_QOS_REAL=1
    clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -pthread $extra \
        tests/test_audio_qos_macos.c -o "out/mac-playability/test-audio-qos-$mode"
    python3 packaging/macos/sign.py "out/mac-playability/test-audio-qos-$mode"
done
python3 - <<'PY'
import os, subprocess
base=dict(os.environ)
base.pop('BB_AUDIO_QOS',None)
for value, mode in [(None,'off'), ('','off'), ('inherit','off'), ('interactive','interactive')]:
    env=base.copy()
    if value is not None: env['BB_AUDIO_QOS']=value
    subprocess.run(['arch','-x86_64','out/mac-playability/test-audio-qos-fake',mode],env=env,check=True)
for value,mode,message in [('bogus','off','must be interactive'),
                           ('interactive','error','failed:'),
                           ('interactive','mismatch','not reported'),
                           ('interactive','get-before','before query failed'),
                           ('interactive','get-after','after query failed')]:
    result=subprocess.run(['arch','-x86_64','out/mac-playability/test-audio-qos-fake',mode],
                          env=dict(base,BB_AUDIO_QOS=value),capture_output=True,text=True)
    assert result.returncode==23, result
    assert message in result.stderr, result.stderr
    print('PASS: loud refusal',mode)
# This tests the real pthread API under the caller's launch policy, not device cadence.
for value in ['inherit','interactive']:
    subprocess.run(['arch','-x86_64','out/mac-playability/test-audio-qos-real',value],
                   env=dict(base,BB_AUDIO_QOS=value),check=True)
PY
