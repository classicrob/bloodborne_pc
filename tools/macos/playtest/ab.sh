#!/bin/bash
# ab.sh NAME ORDER ARM=BUILD_DIR... : interleaved quiet R0 A/B (Opus-Playtest). Each run is r0.sh with --perf.
#   ab.sh async3 "A B C C B A" A=<dir>/5d181e5-PREVIEW-27a B=<dir>/092b1f5-PREVIEW-27b C=<dir>/855be73-PREVIEW-27c
# BUILD_DIR holds bb-probe and SHA256SUMS (checked first). Output: out/macos_route/ab-NAME/ (per-run
# .txt/.sum with the r0 output, App Nap/WarmUp lines and perfsum.py; idle sheets and pixel diffs vs
# the first run of the first arm). Env: WARM (default p23-clinic).
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); cd "$ROOT"
NAME=$1; ORDER=$2; shift 2
[ -n "$NAME" ] && [ -n "$ORDER" ] && [ $# -ge 2 ] || { echo "usage: ab.sh NAME \"A B B A\" A=BUILD_DIR B=BUILD_DIR ..."; exit 2; }
OUT=out/macos_route/ab-$NAME; mkdir -p $OUT
build(){ for kv in "${ARMS[@]}"; do [ "${kv%%=*}" = "$1" ] && { echo "${kv#*=}"; return; }; done; echo "no arm $1" >&2; exit 2; }
ARMS=("$@")
for kv in "${ARMS[@]}"; do (cd "${kv#*=}" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) || { echo "checksum failed: ${kv#*=}"; exit 1; }; done
for a in $ORDER; do build $a >/dev/null || exit 2; done
echo "== $NAME: order $ORDER; power: $(pmset -g | grep -i powermode | tr -s ' '); start $(date -u +%T)"
first=""; declare -i n=0
for a in $ORDER; do
  n=n+1; L=$NAME-$a$n
  WARM=${WARM:-p23-clinic} R0_LABEL=$L R0_EXTRA="--perf" "$HERE/r0.sh" "$(build $a)/bb-probe" > $OUT/$L.txt 2>&1
  R=$(ls -td out/macos_route/*-$L | head -1)
  { echo "##### arm $a: $(basename "$(build $a)"), run $(basename $R)"; grep -v "^heavy: .*waiting" $OUT/$L.txt
    echo "-- App Nap: $(grep -m1 'App Nap' $R/game.log)"; echo "-- WarmUp: $(grep -m2 'WarmUp' $R/game.log | tr '\n' ' ')"
    echo "-- host (thermal, memory pressure, load; before and after):"; cat $R/host.txt 2>/dev/null || echo "MISSING host.txt"
    echo "-- BB_PERF:"; python3 "$HERE/perfsum.py" $R; } > $OUT/$L.sum
  cat $OUT/$L.sum
  [ -z "$first" ] && first=$R
  [ "$R" != "$first" ] && python3 "$HERE/absheet.py" $OUT/sheet-$L.jpg $first $R | tee $OUT/diff-$L.txt
done
echo "== done $(date -u +%T); output $OUT"
