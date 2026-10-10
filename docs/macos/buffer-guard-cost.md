# Guarded buffer load cost

The safe-index load path computes the existing guest/host bounds predicate,
selects either the valid element index or zero, loads that element, then selects
the loaded value or zero. It never constructs an out-of-range pointer. A uniform
nonempty-alias branch handles bindings too short to contain one whole element;
that branch returns zero without a load. Stores, atomics and scalar reads that
must preserve the previous register retain their existing guarded behavior.

Full GCN conformance on the rebased implementation (1c2bd1a, before the cache
bump) matches its macos base job for job: 1076 strict, 461 ambiguous, six documented
MoltenVK push-descriptor cases, and the same loud cases. There are zero failures
and zero invalid modules out of 371. Tiny aliases pass 213/213 cases; the combined
wave64/add_tid, reuse, LDS and specialization controls also pass.

## Quiet-window measurement

M4 Max, macOS 27.2, MoltenVK 1.4.2, Rosetta x86_64, shared production library
9668c1c. The workload uses 65,536 invocations and 128 loads per invocation
(8,388,608 loads), allocated descriptor sets, eight warmup rounds and 60 measured
rounds. Each round alternates the guarded/reference order. Every dispatch's
65,536 outputs must match the reference exactly. All four generated modules
pass `spirv-val` before timing.

| Emitter | Reference median ms | Guarded median ms | Guarded IQR ms | Ratio |
|---|---:|---:|---|---:|
| Production | 0.048792 | 0.109938 | 0.109167–0.110625 | 2.25× |
| Safe-index loads | 0.049166 | 0.103125 | 0.102333–0.104000 | 2.10× |

The candidate reduces guarded time by 6.2% in this load-heavy workload. The
one-minute host load was 6.7–7.4; the five-minute average was 23 while declining.
This reproduces the earlier approximately 0.111/0.053 ms result, rather than the
0.316/0.067 ms measurement taken under contention. It is not a game FPS claim.

The benchmark source is `tests/test_buffer_bounds_benchmark.cpp`. For a comparison,
compile it once against the production library, and once with the candidate
`spirv_emit_context.cpp` and `emit_spirv_context_get_set.cpp` linked into the test.
Use the same stable library for both. Pass an existing output directory to save
SPIR-V and `timestamps.csv`; add `--emit-only` to validate modules before GPU
execution. Individual measurements are committed alongside this report in
`tests/buffer_guard_cost/`.

Apple shader binary version 19 invalidates the former load modules. Native
shader version 13 is unchanged. Static guard elision needs a proof that remains
true for every descriptor reusing the module; descriptor size observed at one
compile is not such a proof.
