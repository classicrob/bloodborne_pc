#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p out/test-alarm
if [ "$(uname -s)" = Darwin ]; then
    clang -arch x86_64 -std=c11 -Wall -Wextra -Werror tests/test_alarm.c -o out/test-alarm/test
    python3 packaging/macos/sign.py out/test-alarm/test
    exec arch -x86_64 out/test-alarm/test
fi
cc -std=c11 -Wall -Wextra -Werror tests/test_alarm.c -o out/test-alarm/test
exec out/test-alarm/test
