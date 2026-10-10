# Buffer integer atomics

This is a shared recompiler correctness change, covering Linux, Windows and macOS.
The GCN2 DATA operand is an unsigned wrap limit for increment/decrement, not an
ignored operand:

| Operation | Stored value | Returned value when GLC is set |
| --- | --- | --- |
| INC | `old >= DATA ? 0 : old + 1` | `old` |
| DEC | `old == 0 || old > DATA ? DATA : old - 1` | `old` |
| SUB | `old - DATA` modulo the operand width | `old` |

INC/DEC carry DATA through the IR and use a compare/exchange retry loop. Each
attempt atomically reads the old value, computes the unsigned predicate and new
value, then compares/exchanges. Only the successful attempt returns its old value.
There is no lock or barrier, and contention retries the operation. The existing
null and bounds guards enclose the complete loop, so an excluded access makes no
memory change and returns zero. Cost is one atomic load and one CAS per attempt,
plus the predicate/arithmetic; other supported integer atomics remain native
single RMW operations. Existing unbounded shared-memory/GDS increments lowered to
buffers continue to use native add/subtract one.

The frontend now handles every integer BUFFER_ATOMIC X2 opcode: SWAP, CMPSWAP,
ADD, SUB, signed/unsigned MIN/MAX, AND, OR, XOR, INC and DEC. The resource passes
classify their storage type and address units as 64-bit, retain side effects even
without GLC, and mark the required native 64-bit atomic capability. With GLC,
both result words are written back. CMPSWAP reads the replacement from the first
two DATA registers and comparison value from the following two registers.

Native 64-bit atomics are required for these X2 operations, including 64-bit CAS
for bounded INC/DEC. The M4/MoltenVK device advertises
`shaderBufferInt64Atomics=false`; such a shader therefore fails with the existing
explicit required-capability diagnostic. Two separate 32-bit operations do not
implement an atomic 64-bit operation. Native-64 SPIR-V validation on this Mac is
compile-only and must not be reported as successful GPU execution.

Floating X2 FCMPSWAP/FMIN/FMAX remain outside this unit. Their unknown-opcode
error names the opcode and instruction category, and shader translation fails.
They have not been replaced by integer operations or narrowed to f32.

## Verification

The independent suite is `tests/gcn_buffer_conformance`. Use its `--tree` mode
with the candidate source tree, the stable shared signed GPU library and
`--only atomic`; every emitted module also passes through `spirv-val`. It compares
registers and memory against its independent GCN2 reference. Native-64 cases on
this Mac are expected to report the explicit capability gap, not a green GPU
result. The existing suite passed 80 strict GPU cases with 12 explicit native-64
capability rejections and zero failures; all seven emitted modules validated.
The independent expanded run passed all 74 bounded INC/DEC cases and all 78
32-bit atomic cases. All 26 integer-X2 opcode/GLC variants compiled with a forced
capability and passed SPIR-V validation; actual M4 execution stopped loudly at
the native-64 capability check, as required. Sol-2 reviewed and approved the
frontend, arithmetic, backend and resource bookkeeping. Shader binary cache
version 15 on Apple and version 12 on Linux/Windows invalidate earlier generated
code. Apple versions 13 and 14 belong to the separate FP64 rejection and push-layout units.

The standalone contention regression is:

```sh
BB_GPU_BUILD=/path/to/stable/out/gpu sh tests/test_buffer_atomic_wrap.sh
```

It compiles the actual buffer atomic emitter, reuses the shared GPU library,
signs/verifies its executable, and launches 4,096 invocations in 64 workgroups
against one counter. Sixteen INC/DEC scenarios cover DATA 0/1/5/UINT_MAX and old
values at zero, equal to or above the limit, and near unsigned overflow. It checks
the complete returned-old-value histogram, final counter and untouched canaries.
All scenarios and both generated modules passed on M4 Max, macOS 27.2, Rosetta,
MoltenVK 1.4.2. No native-64 GPU execution is implied by this 32-bit regression.
