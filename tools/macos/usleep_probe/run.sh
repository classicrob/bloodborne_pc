#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p out/usleep-probe
capture_dir=$(mktemp -d "$PWD/out/usleep-probe/run.XXXXXX")
clang -arch x86_64 -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -pthread \
    tools/macos/usleep_probe/probe.c src/compat_mac.c -I. -Isrc -Wl,-dead_strip -o "$capture_dir/probe"
python3 packaging/macos/sign.py "$capture_dir/probe"
git rev-parse HEAD > "$capture_dir/commit.txt"
uptime > "$capture_dir/host-before.txt"
arch -x86_64 "$capture_dir/probe" "${1:-64}" > "$capture_dir/samples.csv"
uptime > "$capture_dir/host-between.txt"
clang -arch arm64 -std=c11 -O2 -g -Wall -Wextra -Werror -DBB_USLEEP_HOST_ONLY \
    tools/macos/usleep_probe/probe.c -o "$capture_dir/native-probe"
python3 packaging/macos/sign.py "$capture_dir/native-probe"
arch -arm64 "$capture_dir/native-probe" "${1:-64}" > "$capture_dir/native-samples.csv"
uptime > "$capture_dir/host-after.txt"
cat "$capture_dir/samples.csv"
cat "$capture_dir/native-samples.csv"
printf 'Evidence: %s\n' "$capture_dir"
