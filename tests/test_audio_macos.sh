#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
prefix=${BB_DEPS_PREFIX:-"$HOME/bbport-deps/x86_64"}
mkdir -p out/mac-playability
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -I. -Isrc -I"$prefix/include" \
    tests/test_audio_macos.c src/compat_mac.c -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    -lSDL3 -lm -o out/mac-playability/test-audio
arch -x86_64 out/mac-playability/test-audio --timer
arch -x86_64 out/mac-playability/test-audio
python3 - <<'PY'
import os, subprocess
result = subprocess.run(['out/mac-playability/test-audio'],
                        env=dict(os.environ, SDL_AUDIO_DRIVER='bbport-missing-driver'),
                        capture_output=True, text=True)
assert result.returncode != 0
assert 'STOP: macOS audio SDL initialization failed:' in result.stderr, result.stderr
print('PASS: missing SDL driver fails explicitly')
PY
