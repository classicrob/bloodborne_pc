#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
prefix=${BB_DEPS_PREFIX:-"$HOME/bbport-deps/x86_64"}
mkdir -p out/mac-playability
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -I. -Isrc -I"$prefix/include" \
    tests/test_audio_macos.c src/compat_mac.c -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    -lSDL3 -lm -o out/mac-playability/test-audio
python3 packaging/macos/sign.py out/mac-playability/test-audio
arch -x86_64 out/mac-playability/test-audio --timer
# No host audio initialization is allowed in null mode, even with an invalid SDL driver.
SDL_AUDIO_DRIVER=bbport-missing-driver arch -x86_64 out/mac-playability/test-audio --null-clock
SDL_AUDIO_DRIVER=bbport-missing-driver arch -x86_64 out/mac-playability/test-audio --null
# Real CoreAudio output plays a short 440 Hz tone on the default device: opt-in, so build.sh
# --test does not sound on the user's speakers.
if [ "${BB_TEST_AUDIO_DEVICE:-0}" = 1 ]; then
    arch -x86_64 out/mac-playability/test-audio
else
    echo "SKIP (opt-in): real CoreAudio output; BB_TEST_AUDIO_DEVICE=1 plays it"
fi
python3 - <<'PY'
import os, subprocess
env=dict(os.environ, SDL_AUDIO_DRIVER='bbport-missing-driver')
env.pop('BB_AUDIO_OUTPUT', None)
result = subprocess.run(['out/mac-playability/test-audio'],
                        env=env,
                        capture_output=True, text=True)
assert result.returncode != 0
assert 'STOP: macOS audio SDL initialization failed:' in result.stderr, result.stderr
print('PASS: missing SDL driver fails explicitly')
result = subprocess.run(['out/mac-playability/test-audio'],
                        env=dict(env, BB_AUDIO_OUTPUT='invalid'),capture_output=True,text=True)
assert result.returncode != 0
assert 'STOP: BB_AUDIO_OUTPUT must be null or unset' in result.stderr, result.stderr
print('PASS: invalid output selection fails explicitly')
PY
