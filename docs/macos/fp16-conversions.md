# Half conversion modes on Apple

The Apple recompiler handles native half conversions by opcode and direction.
It does not request one global half denorm mode or reject a shader merely because
its F32 and F16/64 MODE fields differ. Half bitcasts do not perform arithmetic
and retain all bits under every MODE.

| MODE denorm16_64 | F32 → F16 output | F16 → F32 input |
|---|---|---|
| 0: flush input and output | Flush half subnormals to signed zero | Flush half subnormals to signed zero |
| 1: allow input, flush output | Flush half subnormals to signed zero | Preserve |
| 2: flush input, allow output | Preserve | Flush half subnormals to signed zero |
| 3: allow input and output | Preserve | Preserve |

Narrowing supports nearest-even rounding. Other rounding modes abort at shader
compilation with the opcode, shader hash, rounding and denorm fields. Widening a
finite half is exact and does not require a rounding-mode override. Native Float16
and signed-zero/Inf/NaN preservation are required for either conversion. Linux
and Windows keep their previous emitter and execution-mode behavior.

The direction interpretation follows the compiler evidence summarized in
[the float-mode audit](float-modes.md). LLVM's
[VOP1 instruction definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/VOP1Instructions.td)
mark half narrowing as using DP rounding, while Mesa's
[FP mode pass](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/amd/compiler/aco_insert_fp_mode.cpp)
tracks half input/output denorm behavior. This is compiler evidence for GCN;
these tests do not measure MODE behavior on a physical PS4. Explicit
`V_CVT_PKRTZ_F16_F32` packing is a separate lowering and is not qualified here.

## Production-emitter qualification

Tested under Rosetta on M4 Max, macOS 27.2, MoltenVK 1.4.2. The device reports
native Float16, half signed-zero/Inf/NaN preservation and RTE; it reports neither
half denorm preserve nor flush execution-mode support. The emitted `OpFConvert`
nonetheless preserves half subnormal values on this configuration. The test
enables the advertised signed-zero preservation mode and does not request an
unsupported denorm execution mode.

`tests/macos_fp16/run.sh` compiles the actual conversion emitter and tests
runtime-loaded operands: positive/negative minimum half subnormals, largest
subnormals, the normal boundary, underflow ties, RTE ties, overflow, signed zero,
infinities, NaNs and bitcasts. Each row's input and three tail canaries must stay
unchanged. Conversion NaNs are checked by classification; bitcast NaN payloads
are checked exactly.

| Implementation | MODE 0 mismatches | MODE 1 | MODE 2 | MODE 3 |
|---|---:|---:|---:|---:|
| Previous `OpFConvert` alone | 10 | 6 | 4 | 0 |
| Direction-specific flush | 0 | 0 | 0 | 0 |

All 208 bit-result checks pass after the change, and all four GPU modules pass
`spirv-val`. Separate full `EmitSPIRV` controls cover all four denorm modes,
rounding modes and conversion/bitcast kinds. Unsupported narrowing rounding is
loud; widening and bitcasts compile without the former mixed-width warnings.

Run against a stable shared library:

```sh
BB_GPU_BUILD=/absolute/path/to/out/gpu \
BB_DEPS_PREFIX=/Users/roberthaisfield/bbport-deps/x86_64 \
bash tests/macos_fp16/run.sh
```

The common MODE 3 conversion adds no instructions. A requested half flush adds
two integer ANDs, an unsigned comparison and a select around representational
bitcasts. It has no branch, memory access or per-draw CPU work. This unit reports
instruction cost rather than a game FPS claim. Apple shader binary version 18
invalidates older conversion modules; native version 13 is unchanged.
