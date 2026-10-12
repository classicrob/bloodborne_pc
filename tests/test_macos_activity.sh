#!/usr/bin/env bash
# The game's NSProcessInfo activity (no App Nap): held for the run and visible as the process's
# "game running" power assertion; BB_APP_NAP=allow leaves it out; any other value is an error.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out/macos-activity
clang -arch x86_64 -std=c11 -Wall -Wextra -Werror -Isrc tests/test_macos_activity.c src/macos_activity.c \
    -lobjc -framework Foundation -o out/macos-activity/test
python3 packaging/macos/sign.py out/macos-activity/test
fail() { echo "macOS activity test FAILED: $*" >&2; exit 1; }
# run [env...]: starts the test (no inherited BB_APP_NAP), waits for its pid line, reads its power
# assertions, then releases it (closes its stdin) and collects its output and exit status.
run() {
    local out=out/macos-activity/out.txt fifo=out/macos-activity/release
    rm -f "$fifo" && mkfifo "$fifo"
    env -u BB_APP_NAP "$@" arch -x86_64 out/macos-activity/test < "$fifo" > "$out" 2>&1 &
    local test=$!
    exec 4> "$fifo"  # the test holds its activity until this closes
    for _ in $(seq 600); do grep -q '^pid ' "$out" && break; kill -0 "$test" 2>/dev/null || break; sleep 0.05; done
    local pid; pid=$(sed -n 's/^pid //p' "$out")
    assertions=$( [[ -n $pid ]] && pmset -g assertions | grep -E "pid $pid\(" || true )
    exec 4>&-
    set +e; wait "$test"; status=$?; set -e
    output=$(cat "$out")
}
run
(( status == 0 )) || fail "default run exited $status: $output"
[[ $output == *'Runtime: App Nap off: NSProcessInfo activity (user initiated, latency critical) for the run'* ]] ||
    fail "no startup line: $output"
[[ $assertions == *'"game running"'* ]] || fail "no \"game running\" power assertion for the process: '$assertions'"
echo "PASS default: activity held for the run ($(head -1 <<< "$assertions" | sed 's/^ *//' | cut -c1-90))"
run BB_APP_NAP=allow
(( status == 0 )) || fail "BB_APP_NAP=allow exited $status: $output"
[[ $output == *'Runtime: App Nap allowed (BB_APP_NAP=allow)'* ]] || fail "no allow line: $output"
[[ $assertions != *'"game running"'* ]] || fail "BB_APP_NAP=allow still took the activity"
echo "PASS BB_APP_NAP=allow: no activity"
run BB_APP_NAP=yes
(( status == 1 )) || fail "BB_APP_NAP=yes exited $status, expected 1"
[[ $output == *'BB_APP_NAP must be unset'* ]] || fail "bad value not explained: $output"
echo "PASS BB_APP_NAP=yes: refused (exit 1)"
