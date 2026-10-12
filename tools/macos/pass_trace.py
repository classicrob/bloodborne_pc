#!/usr/bin/env python3
"""Render pass trace report (BB_PASS_TRACE, gpu/shim/bbport_pass_trace.h).

Reads the JSON lines of a traced replay or game run and prints, per traced frame (means over the
frames):
  - passes by how they began: the same state as the pass ended just before (a resume), the same
    targets with other layouts or clears, or new targets; resumes by what ended the pass before;
  - passes by draws in them (and passes with no draw: clear-only or empty);
  - attachment traffic: bytes loaded (LOAD) and stored (every attachment is stored), by aspect;
  - the fate of every store: read later (sampled, copied, loaded by a later pass), dead (the next
    use clears it or overwrites the whole image), read-only (the pass could not change it), or
    unused until the trace ends (may be read in a later frame: not counted as dead);
  - a model: traffic over a bandwidth plus a cost per pass, against the measured GPU busy, and
    what merging resumes, skipping read-only stores and dead stores, and dropping empty passes
    would save.
Enders are listed as lib+0x... offsets; --lib symbolicates them with atos (x86_64).
"""
import argparse
import collections
import json
import subprocess
import sys

# VkFormat -> (depth bytes, stencil bytes) for depth/stencil formats. Metal keeps the aspects
# apart (D24S8 is emulated as D32S8 on Apple GPUs).
DEPTH_STENCIL = {
    124: (2, 0),  # D16_UNORM
    125: (4, 0),  # X8_D24_UNORM_PACK32
    126: (4, 0),  # D32_SFLOAT
    127: (0, 1),  # S8_UINT
    128: (2, 1),  # D16_UNORM_S8_UINT
    129: (4, 1),  # D24_UNORM_S8_UINT
    130: (4, 1),  # D32_SFLOAT_S8_UINT
}
# Layouts in which an aspect is read-only (VkImageLayout).
DEPTH_READ_ONLY = {4, 1000117000, 1000241001, 1000314000}    # DS RO, D RO/S att, D RO, RO
STENCIL_READ_ONLY = {4, 1000117001, 1000241003, 1000314000}  # DS RO, D att/S RO, S RO, RO
COLOR_READ_ONLY = {1000314000}

READ_BITS = (0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x80 | 0x200 | 0x800 | 0x2000 | 0x8000 |
             0x100000000 | 0x200000000)
ATTACHMENT_BITS = 0x80 | 0x100 | 0x200 | 0x400
STAGE_CLEAR = 0x800000000


def aspect_bytes(att, width, height, layers):
    """Bytes of the attachment's aspect over the render area (0 when unknown)."""
    texel = att["texel_bytes"]
    fmt = att["format"]
    if fmt in DEPTH_STENCIL:
        depth, stencil = DEPTH_STENCIL[fmt]
        texel = stencil if att["slot"] == "stencil" else depth
    return width * height * max(layers, 1) * texel if att["known"] else 0


def read_only(att):
    layout = att["layout"]
    if att["slot"] == "depth":
        return layout in DEPTH_READ_ONLY
    if att["slot"] == "stencil":
        return layout in STENCIL_READ_ONLY
    return layout in COLOR_READ_ONLY


def load(path):
    events = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line:
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError as e:
                    sys.exit(f"{path}:{n}: not JSON ({e})")
    if not events or events[0].get("e") != "start":
        sys.exit(f"{path}: no start record (not a pass trace, or cut short)")
    return events


class Store:
    __slots__ = ("pass_", "slot", "bytes", "fate", "read_only")

    def __init__(self, pass_, slot, nbytes, ro):
        self.pass_, self.slot, self.bytes, self.read_only = pass_, slot, nbytes, ro
        self.fate = None


def analyse(events):
    frames = [e for e in events if e["e"] == "frame"]
    if not frames:
        sys.exit("no complete traced frame (the run ended before the first traced frame end)")
    passes = {}       # pass -> {"begin": ..., "end": ...}
    pending = {}      # uid -> [Store] not yet decided (a store is decided by the next use)
    stores = []
    open_pass = None
    last_end = None   # the end record before the current begin
    begin_kinds = collections.Counter()
    resume_after = collections.Counter()      # (why, ender) of the end before a resume
    same_targets_after = collections.Counter()

    def decide(uid, fate):
        for s in pending.pop(uid, []):
            s.fate = fate

    for e in events:
        kind = e["e"]
        if kind == "begin":
            p = e["pass"]
            passes[p] = {"begin": e, "end": None, "loaded": 0, "stored": 0}
            open_pass = p
            if e["resumed"]:
                begin_kinds["resume (same state)"] += 1
                if last_end:
                    resume_after[(last_end["why"], last_end["ender"])] += 1
            elif e["same_targets"]:
                begin_kinds["same targets (other layouts or clears)"] += 1
                if last_end:
                    same_targets_after[(last_end["why"], last_end["ender"])] += 1
            else:
                begin_kinds["new targets"] += 1
            for att in e["attachments"]:
                if not att["known"]:
                    continue
                if att["load"] == "load":
                    decide(att["uid"], "loaded by a resume" if e["resumed"] else "loaded")
                    passes[p]["loaded"] += aspect_bytes(att, e["width"], e["height"], e["layers"])
                else:
                    decide(att["uid"], "dead: cleared by a later pass")
        elif kind == "end":
            p = e["pass"]
            if p not in passes:
                continue  # began before the trace
            passes[p]["end"] = e
            b = passes[p]["begin"]
            for att in b["attachments"]:
                if not att["known"]:
                    continue
                nbytes = aspect_bytes(att, b["width"], b["height"], b["layers"])
                s = Store(p, att["slot"], nbytes, read_only(att))
                passes[p]["stored"] += nbytes
                stores.append(s)
                if s.read_only:
                    s.fate = "read-only (unchanged)"
                else:
                    pending.setdefault(att["uid"], []).append(s)
            open_pass = None
            last_end = e
        elif kind == "use":
            access, stage, uid = e["access"], e["stage"], e["uid"]
            if access & ATTACHMENT_BITS:
                continue  # the transition to an attachment layout: the begin record decides
            if access & READ_BITS:
                decide(uid, "read (sampled, copied or loaded)")
            elif (stage & STAGE_CLEAR) and e["whole"]:
                decide(uid, "dead: whole image cleared")
            else:
                decide(uid, "written first (partial or unknown extent)")
    for s in stores:
        if s.fate is None:
            s.fate = "unused until the trace ends"
    return frames, passes, stores, begin_kinds, resume_after, same_targets_after


def symbolicate(lib, enders):
    names = {}
    offsets = sorted(x for x in enders if x.startswith("lib+0x"))
    if not lib or not offsets:
        return names
    out = subprocess.run(["atos", "-arch", "x86_64", "-o", lib, "-l", "0"] +
                         [x[4:] for x in offsets], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"atos failed: {out.stderr.strip()}")
    for x, line in zip(offsets, out.stdout.splitlines()):
        name = line.split(" (in ")[0]
        names[x] = name.split("(")[0] if "(" in name else name
    return names


def mib(n):
    return n / (1 << 20)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", help="BB_PASS_TRACE_OUT file")
    parser.add_argument("--lib", help="the libbbgpu.dylib of the run (symbolicates enders)")
    parser.add_argument("--gpu-busy-ms", type=float, help="measured GPU busy per frame (ms)")
    parser.add_argument("--bandwidth", type=float, action="append",
                        help="GB/s for the traffic model (repeatable; default 200, 400, 546)")
    parser.add_argument("--pass-cost-us", type=float, action="append",
                        help="fixed cost per pass in us (repeatable; default 0, 10, 30)")
    parser.add_argument("--json", action="store_true", help="also print a JSON summary")
    args = parser.parse_args()
    bandwidths = args.bandwidth or [200.0, 400.0, 546.0]
    pass_costs = args.pass_cost_us or [0.0, 10.0, 30.0]

    events = load(args.trace)
    frames, passes, stores, begin_kinds, resume_after, same_targets_after = analyse(events)
    nf = len(frames)
    ended = [p for p in passes.values() if p["end"]]
    unknown = sum(1 for p in passes.values() for a in p["begin"]["attachments"]
                  if not a["known"])
    enders = {k[1] for k in list(resume_after) + list(same_targets_after) if k[1]}
    enders |= {p["end"]["ender"] for p in ended if p["end"]["ender"]}
    names = symbolicate(args.lib, enders)

    def label(why, ender):
        return f"{why} {names.get(ender, ender)}".strip()

    print(f"Pass trace: {nf} frame(s); per frame: {len(passes) / nf:.1f} passes, "
          f"{sum(f['draws'] for f in frames) / nf:.0f} draws, "
          f"{sum(f['uses'] for f in frames) / nf:.0f} image accesses"
          + (f"; {unknown / nf:.1f} attachments of unknown views" if unknown else ""))

    print("\nPasses per frame by how they began:")
    for k, v in begin_kinds.most_common():
        print(f"  {v / nf:7.1f}  {k}")
    for title, counter in (("Resumes per frame by what ended the pass before", resume_after),
                           ("Same targets per frame by what ended the pass before",
                            same_targets_after)):
        if counter:
            print(f"\n{title}:")
            for (why, ender), v in counter.most_common(12):
                print(f"  {v / nf:7.1f}  {label(why, ender)}")

    print("\nPasses per frame by what ended them:")
    end_counter = collections.Counter((p["end"]["why"], p["end"]["ender"]) for p in ended)
    for (why, ender), v in end_counter.most_common(14):
        print(f"  {v / nf:7.1f}  {label(why, ender)}")

    print("\nPasses per frame by draws in them:")
    buckets = collections.Counter()
    clear_only = empty = 0
    empty_bytes = 0
    for p in ended:
        d = p["end"]["draws"]
        buckets["0" if d == 0 else "1" if d == 1 else "2-3" if d < 4 else "4-15" if d < 16
                else "16+"] += 1
        if d == 0:
            if any(a["load"] == "clear" for a in p["begin"]["attachments"]):
                clear_only += 1
            else:
                empty += 1
                empty_bytes += p["loaded"] + p["stored"]
    for k in ("0", "1", "2-3", "4-15", "16+"):
        print(f"  {buckets[k] / nf:7.1f}  {k} draws")
    print(f"  (no draw: {clear_only / nf:.1f} clear only, {empty / nf:.1f} empty)")

    loaded = sum(p["loaded"] for p in ended)
    stored = sum(s.bytes for s in stores)
    by_slot = collections.defaultdict(lambda: [0, 0])
    for p in ended:
        b = p["begin"]
        for a in b["attachments"]:
            kind = "color" if a["slot"].startswith("c") else a["slot"]
            n = aspect_bytes(a, b["width"], b["height"], b["layers"])
            by_slot[kind][1] += n
            if a["load"] == "load":
                by_slot[kind][0] += n
    print(f"\nAttachment traffic per frame: {mib(loaded) / nf:.0f} MiB loaded, "
          f"{mib(stored) / nf:.0f} MiB stored")
    for kind, (l, s) in sorted(by_slot.items()):
        print(f"  {kind:8s} {mib(l) / nf:8.0f} MiB loaded {mib(s) / nf:8.0f} MiB stored")

    fates = collections.defaultdict(lambda: [0, 0])
    for s in stores:
        fates[s.fate][0] += 1
        fates[s.fate][1] += s.bytes
    print("\nStores per frame by fate (count, MiB):")
    for fate, (count, nbytes) in sorted(fates.items(), key=lambda kv: -kv[1][1]):
        print(f"  {count / nf:7.1f} {mib(nbytes) / nf:8.0f} MiB  {fate}")

    resumed = [p for p in ended if p["begin"]["resumed"]]
    # Merging a resume into the pass before saves that pass's stores of the shared targets (the
    # resume's loads of them) and one pass.
    merge_bytes = sum(p["loaded"] for p in resumed)
    for p in resumed:
        prev = passes.get(p["begin"]["pass"] - 1)
        if prev and prev["end"]:
            merge_bytes += prev["stored"]
    ro_bytes = fates["read-only (unchanged)"][1]
    dead_bytes = sum(v[1] for k, v in fates.items() if k.startswith("dead"))
    levers = [
        ("merge resumes", merge_bytes, len(resumed)),
        ("no store of read-only aspects", ro_bytes, 0),
        ("no store of dead stores", dead_bytes, 0),
        ("drop empty passes", empty_bytes, empty),
    ]
    print("\nModel per frame (traffic / bandwidth + passes x cost per pass):")
    total = loaded + stored
    for bw in bandwidths:
        for cost in pass_costs:
            ms = total / (bw * 1e9) * 1e3 + len(ended) * cost / 1e3  # over all frames
            print(f"  {bw:5.0f} GB/s, {cost:4.0f} us/pass: {ms / nf:6.2f} ms"
                  + (f" of {args.gpu_busy_ms:.2f} ms GPU busy" if args.gpu_busy_ms else ""))
    # Upper bounds: a resume merges only if the op between does not touch its attachments.
    print("\nLevers per frame (MiB saved; ms at each bandwidth, plus passes saved x cost; "
          "upper bounds):")
    for name, nbytes, saved_passes in levers:
        ms = ", ".join(f"{nbytes / nf / (bw * 1e9) * 1e3:.2f}" for bw in bandwidths)
        print(f"  {name:32s} {mib(nbytes) / nf:8.0f} MiB  {ms} ms"
              + (f"  (+{saved_passes / nf:.1f} passes)" if saved_passes else ""))

    if args.json:
        print(json.dumps({
            "frames": nf, "passes_per_frame": len(passes) / nf,
            "begin_kinds": {k: v / nf for k, v in begin_kinds.items()},
            "loaded_mib": mib(loaded) / nf, "stored_mib": mib(stored) / nf,
            "fates_mib": {k: mib(v[1]) / nf for k, v in fates.items()},
            "levers_mib": {name: mib(b) / nf for name, b, _ in levers},
            "draw_buckets": {k: v / nf for k, v in buckets.items()},
        }, indent=1))


if __name__ == "__main__":
    main()
