# GCN wave64 on Apple 32-wide subgroups

GCN runs 64-lane waves; Apple GPUs run 32-lane SIMD-groups. `Profile::emulate_wave64` (set for
Apple in `vk_pipeline_cache.cpp`) makes `lower_wave64_pass.cpp` rebuild every wave-level operation:

- **Compute**: a wave is 64 consecutive invocations of the workgroup (lane = LocalInvocationIndex
  & 63). Cross-lane operations (Ballot, GroupAny, ReadLane, ReadFirstLane, shuffles) exchange
  values through 32-bit workgroup-shared words between workgroup barriers. This does not depend on
  how the driver groups invocations into SIMD-groups. The exchange needs every invocation of the
  workgroup at the barrier, so it is used only in workgroup-uniform control flow (a fixed-point
  uniformity analysis with control dependence: divergent ifs, loops with divergent exits,
  breaks under divergent ifs).
- **Compute workgroups of at most one subgroup** (<= 32 invocations, e.g. 16x1x1 or 4x4x2) whose
  shader has no lane-index exchange (shuffles): the workgroup is one GCN wave whose lanes beyond the
  workgroup size do not exist. Its wave operations are built from LocalInvocationIndex and
  subgroup reductions over the executing invocations (OpGroupNonUniformIAdd): a ballot is the sum
  of each executing lane's own bit 1 << LocalInvocationIndex, a v_readlane the sum of the target
  lane's value alone. That is exact in any control flow (inactive lanes do not contribute, as on
  GCN) and does not depend on how the driver numbers subgroup invocations (Vulkan does not
  promise SubgroupLocalInvocationId == LocalInvocationIndex without full subgroups). It only
  relies on the whole workgroup being one subgroup (workgroup size <= subgroupSize, 32 on M4).
  v_readlane must name a lane that executes the instruction: an immediate launched lane in
  workgroup-uniform flow, or v_readfirstlane's own selector (`B = Ballot(exec)`,
  `lane = B == 0 ? 0 : FindLsb(B)`, all in the read's block). A non-empty `B` names an
  invocation that executes the read. An empty `B` names lane 0 (v_readfirstlane ignores exec),
  so it is accepted only where lane 0 surely executes (workgroup-uniform flow) or `B` cannot be
  empty (its predicate is the condition of an enclosing if, as inside an `s_cbranch_execz`
  scope). Anything else is a hard error: a lane taken before the divergence, another selector, or
  a bare `s_ff1_i32_b64` (~0 for an empty mask, i.e. lane 63, never launched here). Per-lane
  operations (lane id, v_writelane, inverse ballot, mask find) use the same lowering as larger
  workgroups.
  Host prerequisite: the whole workgroup runs as one subgroup when `workgroup size <=
  subgroupSize` (Vulkan does not promise it; measured on M4 by the suite's `wave1sg` cases in
  1D/2D/3D shapes). This is separate from
  the lane-numbering assumption, which the reductions no longer need. The lowering checks the
  workgroup size the shader declares (its LocalSize), which is what the dispatch uses.
- **Graphics**: a 32-lane subgroup is a legal partially populated wave (lanes 32-63 inactive), so
  subgroup semantics are kept, except reading a lane that may be >= 32, which is a hard error.

Anything that cannot be emulated exactly is a hard error naming the shader (correct or loud).

## Divergent compare masks that only feed scalar-load destinations

A VOP3 compare writes a 64-lane mask into an SGPR pair. Game code (e.g. compute shader
0xa509af23) reuses that SGPR pair as the destination of a later `s_buffer_load`. For an
out-of-range dword, the load is "not performed" (ISA §7.4), so the recompiler carries the
destination's old value: here, the compare mask. When the compare sits in workgroup-divergent
flow, that mask cannot be built without a barrier. It matters only if the scalar load is out of
range.

The ISA is ambiguous about out-of-range scalar dwords: they are left unwritten (the literal
reading) or written as 0 (reading SMEM_ZERO, which `tests/gcn_buffer_conformance/SPEC.md` accepts
too). When **every** workgroup-divergent cross-lane value of a shader is used only as that old
destination value (through phis, 32/64-bit packing and pure bitwise ops such as an
`s_and_saveexec` mask combined with exec), the pass takes the SMEM_ZERO reading for
**all** scalar buffer loads of that shader, then removes the dead masks. A warning names each
such shader (once per compiled module or permutation): `reads out-of-range scalar buffer dwords
as 0 (SMEM_ZERO reading)`. This is a choice
between two accepted readings of the ISA text, not a statement about physical GCN2 behaviour.

Compute shader 0x40229f02 has ten divergent saved-exec masks that reach `s_buffer_load`
destinations only through `s_and_b64`/`s_not_b64`-style ops. Any other use of a divergent mask (a
store, an address, a branch condition, an export) keeps the shader a hard error.

## Considered, unused by this game: taking a divergent ballot at a uniform point

A ballot in a workgroup-divergent block B reached from a workgroup-uniform block U only through ifs
(no loop, break, return or unreachable in between) equals `Ballot(x && c1 && ... && cn)` taken at
the end of U, the ci being the conditions of the ifs open at B: B's invocations are exactly U's
invocations with c1..cn, and the barrier exchange is safe in U. x and every ci must already exist at
the end of U (nothing moved or recomputed). Implemented and tested (branch
`bb/verifier-old-hoist`, commit 7203277), then dropped: across the whole shader corpus it applied to
no shader. The suite keeps the shape ("s_cbranch_execz scope: v_mbcnt(exec) rank + s_bcnt1(exec)",
approved loud) and four controls that must stay loud if it is revived. Suite: group `wave64`, cases "divergent VOP3 compare into s[30:31],
s_buffer_load s30/s40" (in range, out of range, and the two must-be-loud controls).
