# GCN2 buffer conformance suite

An independent check that guest buffer accesses on the macOS (MoltenVK) path behave as on the
PS4's GCN2 GPU, including everything out of range. Written from the AMD Sea Islands ISA (see
`SPEC.md`), not from the recompiler. It is the acceptance test for replacing robustBufferAccess2
with recompiler-emitted bounds checks.

## What runs
- `gcn_asm.h` encodes small GCN compute shaders. Each loads per-lane parameters, executes one
  MUBUF/MTBUF/SMRD instruction under test (exec-masked to the active lanes), and stores its result
  registers.
- `harness.cpp` sends them through the **production** recompiler: `Shader::TranslateProgram`,
  `Backend::SPIRV::EmitSPIRV`, and `StageSpecialization` permutation reuse exactly as
  `PipelineCache::GetProgram` does. Fields outside the specialization key (num_records, add_tid,
  base, soffset) are therefore real run-time inputs, as in the game.
- Each compile runs in a forked child before Vulkan starts. A GPU-library ASSERT/UNREACHABLE (exit
  status 23) or an abort is recorded as **loud** for that job and does not stop the suite.
- A binder mirrors `Rasterizer::BindBuffers` on macOS: the null V# goes to the 64-byte zero
  buffer; otherwise `[AlignDown(offset, minStorageBufferOffsetAlignment), size + adjust)` with
  `buf_offsets[binding] = adjust` and the Apple "mapping is incomplete" assert. Each job has its
  own 64 KiB arena of patterned guest memory inside one VkBuffer, so the bytes just past the bound
  are real memory (canaries) in the same buffer, as in the buffer cache.
- `ref.c` computes the expected registers and memory under every combination of the ambiguous
  readings listed in `SPEC.md`. Stores and atomics run one active lane per job. A job that fails
  with memory differences is re-run alone so writes from other jobs cannot be misattributed.

Also: every generated module goes through `spirv-val` (native, `BB_SPIRV_VAL` to override). A job
whose module is invalid is reported `INVALID-SPIRV` even when MoltenVK happened to return the
expected values. The Apple binder guards of the tree under test (required span, swizzle group,
32-bit range) are detected by build.sh and mirrored.

## Verdicts
| verdict | meaning |
|---|---|
| PASS(strict) | matches; every reading of the ISA agrees |
| PASS(ambiguous) | matches the outcome of at least one reading; the report names it |
| FAIL | no reading produces the observed registers/memory (canary read or write, partial write, wrong value, silent zero where GCN reads memory) |
| LOUD(required) | loud, and no reading's outcome fits inside the bound range, so loud is the only correct choice |
| LOUD(approved) | loud in an orchestrator-approved interim category: structured buffer with nonzero/dynamic soffset, swizzled num_records % index_stride != 0, Apple incomplete mapping |
| LOUD(not required) | loud where an exact result was possible (e.g. an unsupported opcode). Not wrong, but it would stop a game that uses it |
| CRASH / HARNESS-ERROR | signal without a GPU-library message, or a harness limitation |

## Coverage (about 930 jobs)
raw (stride 0) and structured loads/stores: ubyte/sbyte/ushort/sshort/dword/x2/x3/x4; idxen/offen
combinations; inst_offset up to 4092; inline and SGPR soffset (the range shrinks by soffset for raw;
soffset > num_records); the last valid byte/dword/element and the first invalid one; xN per
component; base misaligned to the binding alignment (adjust 4/12) and to 2 bytes; 32-bit wrap of
voffset+inst_offset and of index*stride; num_records 0; GetSize() u32 wrap (stride*num_records =
2^32 or 4); all-zero V#; structured offen-only (offset unchecked, legal reads past the bound);
swizzled (element_size 2/4/8/16, index_stride 8/16, incomplete last group); add_tid (lane id, all
64 lanes, offset checked without idxen); BUFFER_LOAD/STORE_FORMAT with resource dst_sel (incl.
constant 0/1) and TBUFFER (instruction format) all-or-nothing; resource data format INVALID
(MUBUF unbound, TBUFFER ambiguous, s_buffer_load unaffected, including one V# shared by
s_buffer_load and buffer_load); S_BUFFER_LOAD_DWORD/x4 with immediate and SGPR offsets (per-dword
clamp, `>` for stride != 0, unwritten SDST); atomics add/cmpswap/umax/inc/dec/swap/sub/add_x2,
all-or-nothing, with return; specialization reuse across num_records and add_tid; INC/DEC wrap
semantics (group `atomic-semantics`, not a bounds property). Group `wave64`: GCN 64-lane wave
semantics on 32-wide Apple subgroups (lane id via v_mbcnt, VOPC ballot read as VCC/s_mov_b64/
s_bcnt1, v_mbcnt over a mask, v_cndmask, v_readlane with constant/SGPR lanes 0..63,
v_readfirstlane, exec read as a value and s_bcnt1 exec under saveexec). Group `vertex-fetch`: a
vertex shader calling a fetch shader (s_swappc_b64; V# via s_load_dwordx4 from a table pointer;
BUFFER/TBUFFER_LOAD_FORMAT_XYZW idxen) in an indexed draw whose indices run past num_records and
up to 0xFFFFFF80+, with rasterization discarded. Vertex input is mirrored from
GraphicsPipeline::GetVertexInputs/ResolveVertexBuffers (dynamic vertex input when available), or
none when the tree pulls vertices in the shader (`Profile::emulate_vertex_fetch`). Group `srt`: V#
and a constant read through an extended-user-data pointer. Groups `stage-compile` and
`dead-access`: fragment/vertex shaders whose only buffer access is s_buffer_load, live or dead,
plus a compute load whose result is never used (compile-only; an abort is a FAIL).

`--compile-only FILE --stage cs|vs|fs [--ud v0,v1,...] [--fs-export N]` runs one real dumped shader
through the recompiler and spirv-val, without Vulkan. `--disasm FILE` lists its wave-level
instructions.

## Build and run (x86_64 under Rosetta; at most one helper child at a time)
```sh
GPU=<worktree with the built GPU library, e.g. Sol-1's>
# Prefer the --tree form below for every run, including macos itself: dylib mode is only valid when
# this checkout's recompiler headers match the dylib's sources exactly (BufferResource layout etc.).
# recompiler inside the shared dylib:
BB_GPU_BUILD=$GPU/out/gpu BB_GPU_THIRD_PARTY=$GPU/gpu/third_party tests/gcn_buffer_conformance/build.sh
# a branch's recompiler at a fixed commit, compiled into the test executable:
mkdir -p out/gcnconf/trees/C && git archive C gpu/shadps4 gpu/shim gpu/third_party/sirit src/compat_mac.h | tar -x -C out/gcnconf/trees/C
BB_TREE_NAME=C BB_GPU_BUILD=$GPU/out/gpu BB_GPU_THIRD_PARTY=$GPU/gpu/third_party \
    tests/gcn_buffer_conformance/build.sh --tree out/gcnconf/trees/C
# run
VK_DRIVER_FILES=$HOME/bbport-deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json \
    out/gcnconf/gcn-conformance-<name> --out out/gcnconf --label <name> [--only SUBSTRING] [--no-rba]
```
The whole suite takes under a minute. It exits 0 only if nothing FAILs, CRASHes or hits a harness
error. The full report is written to `out/gcnconf/report-<label>.txt`. `--no-rba` creates the
device without robustBufferAccess (production enables it), which exposes the shader's own checks.
The device's capabilities (subgroup size, storage alignment, int64, robustness2) are queried in a
helper child, and the Profile is filled the way `vk_pipeline_cache.cpp` fills it. When the tree
under test has `Profile::emulate_robust_buffer_access`, it is set to `!robustBufferAccess2`.
