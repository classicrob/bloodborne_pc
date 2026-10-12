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
assert [(f["passes"], f["draws"], f["uses"], f["binds"]) for f in frames] == [
    (3, 4, 2, 1), (2, 0, 0, 0)], frames
binds = [e for e in events if e["e"] == "bind"]
assert binds == [{"e": "bind", "frame": 1, "pass": 1, "uid": 42}], binds
begins = [e for e in events if e["e"] == "begin"]
assert [b["resumed"] for b in begins] == [0, 1, 0, 0, 0]
assert begins[4]["attachments"][0]["known"] == 0  # a handle noted for an earlier pass
assert begins[0]["attachments"][1] == {"slot": "depth", "uid": 2, "known": 1, "format": 130,
    "texel_bytes": 5, "width": 100, "height": 100, "layer": 0, "layers": 1, "level": 0,
    "samples": 1, "layout": 3, "load": "clear"}, begins[0]["attachments"][1]
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
# Pass 1's color store is cleared by pass 3, but passes 2 and 4 had unresolved targets between.
assert fates == {
    "read (sampled, copied or loaded)": 40000,
    "loaded by a resume": 50000,
    "dead candidate: whole image cleared": 50000,
    "read-only candidate (unchanged)": 40000,
    "cleared after an unresolved pass (not dead: it may have read it)": 40000,
    "unused until the trace ends": 40000,
}, fates
assert summary["unresolved_passes"] == 2
# The resume (pass 1) is excluded: pass 0's color was sampled between pass 0 and pass 1.
assert summary["merge"] == {} and summary["merge_excluded"] == 1, summary
levers = {k: bytes2(v) for k, v in summary["levers_mib"].items()}
assert levers == {"no store of read-only candidates": 40000,
                  "no store of dead candidates": 50000, "drop empty passes": 80000}, levers
# Pass 2 uses pass 1's view handles in a read-only layout, pass 4 pass 3's handle without its
# clear: same targets, not resumes.
assert summary["begin_kinds"] == {"new targets": 1.0, "resume (same state)": 0.5,
                                  "same targets (other layouts or clears)": 1.0}, summary
assert summary["draw_buckets"] == {"2-3": 0.5, "1": 0.5, "0": 1.5}, summary
assert "(no draw: 0.5 clear only, 1.0 empty)" in text
assert "1.0 attachment aspects not known" in text
assert "1 pass(es) begun before the trace (not counted)" in text
# Known/all attachments: new = pass 0 (color, depth, stencil) + pass 3 (color); resume = pass 1
# (color, depth, stencil); same targets = pass 2 (unknown color, depth) + pass 4 (unknown color).
assert summary["coverage"] == {"new targets": [4, 4], "resume": [3, 3],
                               "same targets": [1, 3]}, summary["coverage"]
assert "Attachment coverage: 8 of 10 attachment aspects known (80% of aspects" in text
EOF
# The analyzer's fate rules on a hand-built trace (one frame).
python3 - "$out/rules.jsonl" <<'EOF'
import json, sys
def att(slot, uid, load, level=0, layer=0, layers=1, samples=1, known=1, fmt=130, texel=5):
    return {"slot": slot, "uid": uid, "known": known, "format": fmt, "texel_bytes": texel,
            "width": 100, "height": 100, "layer": layer, "layers": layers, "level": level,
            "samples": samples, "layout": 3, "load": load}
def color(uid, load, **kw):
    return att("c0", uid, load, fmt=37, texel=4, **kw)
def begin(p, atts, w=100, h=100, resumed=0):
    return {"e": "begin", "frame": 1, "pass": p, "width": w, "height": h, "layers": 1,
            "resumed": resumed, "same_targets": resumed, "attachments": atts}
def end(p, why, ender=""):
    return {"e": "end", "frame": 1, "pass": p, "why": why, "ender": ender, "draws": 1}
ds_clear = [att("depth", 7, "clear"), att("stencil", 7, "load")]
events = [
    {"e": "start", "first": 1, "count": 1, "open_at_start": 0},
    # p0: depth+stencil loaded, 4x MSAA color never used again.
    begin(0, [att("depth", 7, "load"), att("stencil", 7, "load"), color(8, "load", samples=4)]),
    end(0, "target change"),
    begin(1, ds_clear), end(1, "barrier", "lib+0x10"),       # depth cleared, stencil loaded
    begin(2, ds_clear, resumed=1), end(2, "other", "lib+0x20"),  # resume, nothing between
    {"e": "bind", "frame": 1, "pass": 3, "uid": 7},           # p2 depth and stencil: read
    begin(3, [color(9, "clear", level=1)], w=50, h=50), end(3, "other"),
    begin(4, [color(9, "clear")]), end(4, "other"),           # other level: p3 partial
    begin(5, [color(9, "clear")], w=50, h=50), end(5, "other"),  # smaller area: p4 partial
    begin(6, [color(9, "load", known=0)]), end(6, "other"),   # a proxy of uid 9: p5 used
    {"e": "frame", "frame": 1, "passes": 7, "draws": 7, "uses": 0, "binds": 1,
     "open_at_end": 0},
]
with open(sys.argv[1], "w") as f:
    f.write("".join(json.dumps(e) + "\n" for e in events))
EOF
python3 tools/macos/pass_trace.py "$out/rules.jsonl" --json > "$out/rules.txt"
python3 - "$out/rules.txt" <<'EOF'
import json, sys
text = open(sys.argv[1]).read()
summary = json.loads(text[text.index("\n{") + 1:])
fates = {k: round(v * (1 << 20)) for k, v in summary["fates_mib"].items()}
assert fates == {
    "dead candidate: cleared by a later pass": 80000,  # p0 and p1 depth, not their stencil
    "loaded": 10000,                                    # p0 stencil (p1 loads it)
    "loaded by a resume": 10000,                        # p1 stencil
    "unused until the trace ends": 160000,              # p0 color: 100x100 x4 bytes x4 samples
    "read (bound to a draw or dispatch)": 50000,        # p2 depth and stencil
    "written first (partial or unknown extent)": 50000,  # p3 (other level), p4 (smaller area)
    "used through a redirect or proxy (unknown)": 10000,  # p5
}, fates
barrier = "barrier enders (needs the barrier's producer moved out of the pass)"
assert summary["merge"] == {barrier: [1, 60000, 0]}, summary["merge"]  # p2 load + p1 stores
assert summary["unresolved_passes"] == 0
EOF
echo "pass-trace-test: PASS"
