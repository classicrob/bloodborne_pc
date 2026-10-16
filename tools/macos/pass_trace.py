#!/usr/bin/env python3
"""Render pass trace report (BB_PASS_TRACE, gpu/shim/bbport_pass_trace.h).

Reads the JSON lines of a traced replay or game run and prints, per traced frame (means over the
frames):
  - passes by how they began: the same state as the pass ended just before (a resume), the same
    targets with other layouts or clears, or new targets; resumes by what ended the pass before;
  - passes by draws in them (and passes with no draw: clear-only or empty);
  - attachment traffic: bytes loaded (LOAD) and stored (every attachment is stored), by aspect;
  - the fate of every store, per image and aspect (color, depth, stencil): read later (a
    requested read, an image bound to a draw or dispatch, a load by a later pass), a dead
    candidate (the next event clears the same level, layers and area), a read-only candidate
    (the pass's layout could not change it), written first (partial), or unused until the trace
    ends (may be read in a later frame: not dead). Candidates only: an image use recorded
    straight into a command buffer without Runtime::Transit or a binding is not seen, and a store
    pending across a pass with unresolved targets is never called dead;
  - attachments of unknown views (redirects, proxies, helpers, passes begun from a cached render
    state without the rasterizer's full rebuild) are counted but carry no bytes: the coverage
    line gives known/all by begin kind, so sparse coverage does not read as low traffic;
  - a model: logical traffic (Apple GPUs compress render targets losslessly, so measured
    bandwidth is lower) over a bandwidth plus a cost per pass, against the measured GPU busy, and
    upper bounds, not summed, for merging resumes (by ender class; excluded when an access or
    bind between touched the attachments), skipping read-only and dead-candidate stores, and
    dropping empty passes.
Enders are listed as lib+0x... offsets; --lib symbolicates them with atos (x86_64).
"""
import argparse
import collections
import json
import subprocess
import sys

# VkFormat -> (depth bytes, stencil bytes) for depth/stencil formats as Metal stores them: the
# aspects apart, D24S8 and D16S8 as D32Float_S8.
DEPTH_STENCIL = {
    124: (2, 0),  # D16_UNORM
    125: (4, 0),  # X8_D24_UNORM_PACK32
    126: (4, 0),  # D32_SFLOAT
    127: (0, 1),  # S8_UINT
    128: (4, 1),  # D16_UNORM_S8_UINT (no Metal format: MoltenVK uses D32Float_S8)
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
    samples = max(att.get("samples", 1), 1)
    return width * height * max(layers, 1) * samples * texel if att["known"] else 0


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
    __slots__ = ("pass_", "slot", "uid", "bytes", "fate", "read_only", "level", "layer",
                 "layers", "width", "height", "tainted")

    def __init__(self, pass_, att, begin, nbytes):
        self.pass_, self.slot, self.uid, self.bytes = pass_, att["slot"], att["uid"], nbytes
        self.read_only = read_only(att)
        self.level, self.layer = att["level"], att.get("layer", 0)
        self.layers = max(att["layers"], 1)
        self.width, self.height = begin["width"], begin["height"]
        self.tainted = False  # an unresolved pass ran after it: it cannot be called dead
        self.fate = None


def aspect(att):
    return "color" if att["slot"].startswith("c") else att["slot"]


def covers(store, att, begin):
    """Whether a later pass clearing `att` overwrites all that `store` stored."""
    return (att["level"] == store.level and att.get("layer", 0) <= store.layer and
            att.get("layer", 0) + max(att["layers"], 1) >= store.layer + store.layers and
            begin["width"] >= store.width and begin["height"] >= store.height)


DEAD_CLEARED = "dead candidate: cleared by a later pass"
DEAD_WHOLE = "dead candidate: whole image cleared"
NOT_DEAD_TAINTED = "cleared after an unresolved pass (not dead: it may have read it)"
PARTIAL = "written first (partial or unknown extent)"
READ_ONLY = "read-only candidate (unchanged)"


def analyse(events):
    frames = [e for e in events if e["e"] == "frame"]
    if not frames:
        sys.exit("no complete traced frame (the run ended before the first traced frame end)")
    passes = {}       # pass -> {"begin": ..., "end": ..., "loaded", "stored"}
    pending = {}      # (uid, aspect) -> [Store] not yet decided (decided by the next use)
    stores = []
    last_end = None   # the end record before the current begin
    touched = set()   # uids accessed or bound since last_end (attachment transitions aside)
    begin_kinds = collections.Counter()
    partial_ends = 0
    unresolved_passes = 0
    resume_after = collections.Counter()      # (why, ender) of the end before a resume
    same_targets_after = collections.Counter()
    resumes = []      # {"why", "ender", "bytes", "touched", "unresolved"} per resume

    def settle(store, fate):
        if fate.startswith("dead candidate") and store.tainted:
            fate = NOT_DEAD_TAINTED
        store.fate = fate

    def decide_key(key, fate):
        for store in pending.pop(key, []):
            settle(store, fate)

    def decide_uid(uid, fate):
        for key in [k for k in pending if k[0] == uid]:
            decide_key(key, fate)

    for e in events:
        kind = e["e"]
        if kind == "begin":
            p = e["pass"]
            passes[p] = {"begin": e, "end": None, "loaded": 0, "stored": 0}
            if e["resumed"]:
                begin_kinds["resume (same state)"] += 1
                if last_end:
                    resume_after[(last_end["why"], last_end.get("ender", ""))] += 1
            elif e["same_targets"]:
                begin_kinds["same targets (other layouts or clears)"] += 1
                if last_end:
                    same_targets_after[(last_end["why"], last_end.get("ender", ""))] += 1
            else:
                begin_kinds["new targets"] += 1
            unresolved = False
            for att in e["attachments"]:
                if not att["known"]:
                    if att["uid"]:
                        # A redirect or proxy of a known guest image: other storage, used.
                        decide_uid(att["uid"], "used through a redirect or proxy (unknown)")
                    else:
                        unresolved = True
                    continue
                key = (att["uid"], aspect(att))
                if att["load"] == "load":
                    decide_key(key, "loaded by a resume" if e["resumed"] else "loaded")
                    passes[p]["loaded"] += aspect_bytes(att, e["width"], e["height"], e["layers"])
                else:
                    for store in pending.pop(key, []):
                        settle(store, DEAD_CLEARED if covers(store, att, e) else PARTIAL)
            if unresolved:
                # Its targets are unknown: any pending store may have been read by it.
                unresolved_passes += 1
                for waiting in pending.values():
                    for store in waiting:
                        store.tainted = True
            if e["resumed"] and last_end and not last_end.get("partial"):
                uids = {a["uid"] for a in e["attachments"] if a["uid"]}
                resumes.append({"why": last_end["why"], "ender": last_end.get("ender", ""),
                                "pass": p, "touched": bool(uids & touched),
                                "unresolved": unresolved})
        elif kind == "end":
            touched = set()
            last_end = e
            if e.get("partial"):
                partial_ends += 1  # began before the trace: attachments and draws unknown
                continue
            p = e["pass"]
            if p not in passes:
                continue
            passes[p]["end"] = e
            b = passes[p]["begin"]
            for att in b["attachments"]:
                if not att["known"]:
                    continue
                nbytes = aspect_bytes(att, b["width"], b["height"], b["layers"])
                store = Store(p, att, b, nbytes)
                passes[p]["stored"] += nbytes
                stores.append(store)
                if store.read_only:
                    store.fate = READ_ONLY
                else:
                    pending.setdefault((att["uid"], aspect(att)), []).append(store)
        elif kind == "use":
            access, stage, uid = e["access"], e["stage"], e["uid"]
            if access & ATTACHMENT_BITS:
                continue  # the transition to an attachment layout: the begin record decides
            touched.add(uid)
            if access & READ_BITS:
                decide_uid(uid, "read (sampled, copied or loaded)")
            elif (stage & STAGE_CLEAR) and e["whole"]:
                decide_uid(uid, DEAD_WHOLE)
            else:
                decide_uid(uid, PARTIAL)
        elif kind == "bind":
            touched.add(e["uid"])
            decide_uid(e["uid"], "read (bound to a draw or dispatch)")
    for store in stores:
        if store.fate is None:
            store.fate = "unused until the trace ends"
    return {"frames": frames, "passes": passes, "stores": stores, "begin_kinds": begin_kinds,
            "resume_after": resume_after, "same_targets_after": same_targets_after,
            "partial_ends": partial_ends, "unresolved_passes": unresolved_passes,
            "resumes": resumes}


TRANSFER_ENDERS = ("CopyBuffer", "FillBuffer", "InlineData", "UploadImage", "CopyImage",
                   "ClearImage", "DownloadImage", "RefreshImage", "BlitImage", "ResolveImage")


def ender_class(why, name):
    if why == "barrier":
        return "barrier enders (needs the barrier's producer moved out of the pass)"
    if why == "other":
        if name.startswith("lib+0x"):
            return "other enders (unclassified: pass --lib)"
        if any(t in name for t in TRANSFER_ENDERS):
            return "transfer enders (what pass hoisting moves)"
        return "dispatch, HLE and other enders"
    return f"{why} enders"


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
    r = analyse(events)
    frames, passes, stores = r["frames"], r["passes"], r["stores"]
    nf = len(frames)
    ended = [p for p in passes.values() if p["end"]]
    unknown = sum(1 for p in passes.values() for a in p["begin"]["attachments"]
                  if not a["known"])
    enders = {k[1] for k in list(r["resume_after"]) + list(r["same_targets_after"]) if k[1]}
    enders |= {p["end"]["ender"] for p in ended if p["end"]["ender"]}
    names = symbolicate(args.lib, enders)

    def label(why, ender):
        return f"{why} {names.get(ender, ender)}".strip()

    print(f"Pass trace: {nf} frame(s); per frame: {len(passes) / nf:.1f} passes, "
          f"{sum(f['draws'] for f in frames) / nf:.0f} draws, "
          f"{sum(f['uses'] for f in frames) / nf:.0f} requested image accesses, "
          f"{sum(f.get('binds', 0) for f in frames) / nf:.0f} image binds"
          + (f"; {unknown / nf:.1f} attachment aspects not known" if unknown else "")
          + (f"; {r['partial_ends']} pass(es) begun before the trace (not counted)"
             if r["partial_ends"] else ""))

    print("\nPasses per frame by how they began:")
    for k, v in r["begin_kinds"].most_common():
        print(f"  {v / nf:7.1f}  {k}")
    for title, counter in (("Resumes per frame by what ended the pass before",
                            r["resume_after"]),
                           ("Same targets per frame by what ended the pass before",
                            r["same_targets_after"])):
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
            n = aspect_bytes(a, b["width"], b["height"], b["layers"])
            by_slot[aspect(a)][1] += n
            if a["load"] == "load":
                by_slot[aspect(a)][0] += n
    # Coverage: traffic counts known attachment aspects only (unknown ones carry no bytes), so
    # sparse coverage must not read as low traffic. Counted in aspects, not bytes.
    coverage = collections.defaultdict(lambda: [0, 0])  # begin kind -> [known, all]
    for p in ended:
        b = p["begin"]
        kind = ("resume" if b["resumed"] else "same targets" if b["same_targets"]
                else "new targets")
        for a in b["attachments"]:
            coverage[kind][0] += 1 if a["known"] else 0
            coverage[kind][1] += 1
    known_all = sum(v[0] for v in coverage.values())
    all_all = sum(v[1] for v in coverage.values())
    print(f"\nAttachment coverage: {known_all} of {all_all} attachment aspects known "
          f"({100 * known_all / max(all_all, 1):.0f}% of aspects, not of bytes); by begin kind: "
          + ", ".join(f"{k} {v[0]}/{v[1]}" for k, v in sorted(coverage.items())))
    print(f"Attachment traffic per frame (known aspects only; logical bytes, before Apple's "
          f"lossless compression): {mib(loaded) / nf:.0f} MiB loaded, "
          f"{mib(stored) / nf:.0f} MiB stored")
    for kind, (l, s) in sorted(by_slot.items()):
        print(f"  {kind:8s} {mib(l) / nf:8.0f} MiB loaded {mib(s) / nf:8.0f} MiB stored")

    fates = collections.defaultdict(lambda: [0, 0])
    for s in stores:
        fates[s.fate][0] += 1
        fates[s.fate][1] += s.bytes
    print(f"\nStores per frame by fate (count, MiB); {r['unresolved_passes'] / nf:.1f} passes "
          f"per frame had unresolved targets (stores pending across them are not dead):")
    for fate, (count, nbytes) in sorted(fates.items(), key=lambda kv: -kv[1][1]):
        print(f"  {count / nf:7.1f} {mib(nbytes) / nf:8.0f} MiB  {fate}")

    # Merging a resume into the pass before saves that pass's stores (the resume's loads of the
    # same targets) and one pass. Excluded when anything between them accessed or bound one of
    # its attachments; split by the ender's class, which are not equally mergeable.
    merge = collections.defaultdict(lambda: [0, 0, 0])  # class -> [resumes, bytes, unresolved]
    excluded = 0
    for m in r["resumes"]:
        if m["touched"]:
            excluded += 1
            continue
        cls = ender_class(m["why"], names.get(m["ender"], m["ender"]))
        prev = passes.get(m["pass"] - 1)
        nbytes = passes[m["pass"]]["loaded"] + (prev["stored"] if prev and prev["end"] else 0)
        merge[cls][0] += 1
        merge[cls][1] += nbytes
        merge[cls][2] += 1 if m["unresolved"] else 0
    ro_bytes = fates.get(READ_ONLY, [0, 0])[1]
    dead_bytes = sum(v[1] for k, v in fates.items() if k.startswith("dead candidate"))
    levers = [(f"merge resumes, {cls}", v[1], v[0]) for cls, v in sorted(merge.items())]
    levers += [
        ("no store of read-only candidates", ro_bytes, 0),
        ("no store of dead candidates", dead_bytes, 0),
        ("drop empty passes", empty_bytes, empty),
    ]
    print("\nModel per frame (logical traffic / bandwidth + passes x cost per pass):")
    total = loaded + stored
    for bw in bandwidths:
        for cost in pass_costs:
            ms = total / (bw * 1e9) * 1e3 + len(ended) * cost / 1e3  # over all frames
            print(f"  {bw:5.0f} GB/s, {cost:4.0f} us/pass: {ms / nf:6.2f} ms"
                  + (f" of {args.gpu_busy_ms:.2f} ms GPU busy" if args.gpu_busy_ms else ""))
    print("\nLevers per frame, upper bounds on logical traffic, not summed (MiB; ms at each "
          "bandwidth; passes saved):")
    for name, nbytes, saved_passes in levers:
        ms = ", ".join(f"{nbytes / nf / (bw * 1e9) * 1e3:.2f}" for bw in bandwidths)
        print(f"  {name:72s} {mib(nbytes) / nf:8.0f} MiB  {ms} ms"
              + (f"  ({saved_passes / nf:.1f} passes)" if saved_passes else ""))
    if excluded:
        print(f"  ({excluded / nf:.1f} resumes per frame excluded: an access or bind between "
              f"touched their attachments)")
    unresolved_resumes = sum(v[2] for v in merge.values())
    if unresolved_resumes:
        print(f"  ({unresolved_resumes / nf:.1f} counted resumes per frame have unresolved "
              f"targets: their bytes are missing from the merge bounds)")

    if args.json:
        print(json.dumps({
            "frames": nf, "passes_per_frame": len(passes) / nf,
            "partial_ends": r["partial_ends"], "unresolved_passes": r["unresolved_passes"],
            "begin_kinds": {k: v / nf for k, v in r["begin_kinds"].items()},
            "loaded_mib": mib(loaded) / nf, "stored_mib": mib(stored) / nf,
            "coverage": {k: v for k, v in coverage.items()},
            "fates_mib": {k: mib(v[1]) / nf for k, v in fates.items()},
            "merge": {k: v for k, v in merge.items()}, "merge_excluded": excluded,
            "levers_mib": {name: mib(b) / nf for name, b, _ in levers},
            "draw_buckets": {k: v / nf for k, v in buckets.items()},
        }, indent=1))


if __name__ == "__main__":
    main()
