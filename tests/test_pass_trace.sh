#!/bin/sh
# Pass trace (gpu/shim/bbport_pass_trace.h) and its report (tools/macos/pass_trace.py): the driver
# (tests/test_pass_trace.cpp) traces a known sequence; the file and the report are checked against
# numbers worked out by hand. Malformed settings must stop.
set -eu
cd "$(dirname "$0")/.."
out=out/pass-trace-test
mkdir -p "$out"
"${CXX:-clang++}" -arch x86_64 -std=c++20 -O1 -Wall -Wextra -Werror -Igpu/shim \
    tests/test_pass_trace.cpp -o "$out/driver"
codesign --force --sign - "$out/driver" 2>/dev/null

# Malformed settings stop (and write nothing).
for bad in 0 x 3+0 3+65 1+ +2; do
    if { BB_PASS_TRACE=$bad BB_PASS_TRACE_OUT="$out/bad.jsonl" "$out/driver" >/dev/null 2>"$out/bad.err"; } 2>/dev/null; then
        echo "FAIL: BB_PASS_TRACE=$bad ran"; exit 1
    fi
    grep -q "STOP: BB_PASS_TRACE=$bad" "$out/bad.err" || { echo "FAIL: BB_PASS_TRACE=$bad: no STOP"; exit 1; }
done
if { BB_PASS_TRACE=1 "$out/driver" >/dev/null 2>"$out/bad.err"; } 2>/dev/null; then
    echo "FAIL: BB_PASS_TRACE without BB_PASS_TRACE_OUT ran"; exit 1
fi
grep -q "STOP: BB_PASS_TRACE is set without BB_PASS_TRACE_OUT" "$out/bad.err" ||
    { echo "FAIL: no STOP without BB_PASS_TRACE_OUT"; exit 1; }

rm -f "$out/trace.jsonl"
BB_PASS_TRACE=1+2 BB_PASS_TRACE_OUT="$out/trace.jsonl" "$out/driver" > "$out/driver.out"
grep -q "Pass trace: frames 1..2 written to $out/trace.jsonl" "$out/driver.out" ||
    { echo "FAIL: no end-of-trace line"; cat "$out/driver.out"; exit 1; }
python3 tools/macos/pass_trace.py "$out/trace.jsonl" --gpu-busy-ms 1 --json > "$out/report.txt"

python3 - "$out/trace.jsonl" "$out/report.txt" <<'EOF'
import json, sys
events = [json.loads(l) for l in open(sys.argv[1])]
kinds = [e["e"] for e in events]
assert kinds[0] == "start" and events[0]["open_at_start"] == 1, events[0]
partial = [e for e in events if e["e"] == "end" and e.get("partial")]
assert len(partial) == 1 and partial[0]["why"] == "other", partial
assert kinds.count("begin") == 5 and kinds.count("end") == 6, kinds  # nothing before or after
frames = [e for e in events if e["e"] == "frame"]
assert [(f["passes"], f["draws"], f["uses"]) for f in frames] == [(3, 4, 2), (2, 0, 0)], frames
begins = [e for e in events if e["e"] == "begin"]
assert [b["resumed"] for b in begins] == [0, 1, 0, 0, 0]
assert begins[4]["attachments"][0]["known"] == 0  # a handle noted for an earlier pass
assert begins[0]["attachments"][1] == {"slot": "depth", "uid": 2, "known": 1, "format": 130,
    "texel_bytes": 5, "width": 100, "height": 100, "layers": 1, "level": 0, "layout": 3,
    "load": "clear"}, begins[0]["attachments"][1]
assert begins[2]["attachments"][0]["known"] == 0
ends = [e for e in events if e["e"] == "end" and not e.get("partial")]
assert [e["why"] for e in ends] == ["barrier", "other", "target change", "submit",
                                    "target change"]
assert ends[0]["ender"].startswith("lib+0x") and ends[2]["ender"] == ""
assert [e["draws"] for e in ends] == [3, 1, 0, 0, 0]

text = open(sys.argv[2]).read()
summary = json.loads(text[text.index("\n{") + 1:])
B = 1 << 20
def bytes2(mib):  # per-frame MiB -> bytes over both frames
    return round(mib * B * 2)
assert summary["frames"] == 2 and summary["passes_per_frame"] == 2.5
assert summary["partial_ends"] == 1
assert bytes2(summary["loaded_mib"]) == 180000, summary
assert bytes2(summary["stored_mib"]) == 260000, summary
fates = {k: bytes2(v) for k, v in summary["fates_mib"].items()}
assert fates == {
    "read (sampled, copied or loaded)": 40000,
    "loaded by a resume": 50000,
    "dead candidate: whole image cleared": 50000,
    "read-only candidate (unchanged)": 40000,
    "dead candidate: cleared by a later pass": 40000,
    "unused until the trace ends": 40000,
}, fates
levers = {k: bytes2(v) for k, v in summary["levers_mib"].items()}
assert levers == {"merge resumes": 180000, "no store of read-only candidates": 40000,
                  "no store of dead candidates": 90000, "drop empty passes": 80000}, levers
# Pass 2 uses pass 1's view handles in a read-only layout, pass 4 pass 3's handle without its
# clear: same targets, not resumes.
assert summary["begin_kinds"] == {"new targets": 1.0, "resume (same state)": 0.5,
                                  "same targets (other layouts or clears)": 1.0}, summary
assert summary["draw_buckets"] == {"2-3": 0.5, "1": 0.5, "0": 1.5}, summary
assert "(no draw: 0.5 clear only, 1.0 empty)" in text
assert "1.0 attachments of unknown views" in text
assert "1 pass(es) begun before the trace (not counted)" in text
# Known/all attachments: new = pass 0 (color, depth, stencil) + pass 3 (color); resume = pass 1
# (color, depth, stencil); same targets = pass 2 (unknown color, depth) + pass 4 (unknown color).
assert summary["coverage"] == {"new targets": [4, 4], "resume": [3, 3],
                               "same targets": [1, 3]}, summary["coverage"]
assert "Attachment coverage: 8 of 10 attachments known (80%)" in text
EOF
echo "pass-trace-test: PASS"
