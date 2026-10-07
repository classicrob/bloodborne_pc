# GCN2 float modes on Apple

Measured on macOS 27.2 (26B5101f), Apple M4 Max, Rosetta x86_64, the shared
MoltenVK 1.4.2 (`driverVersion=10402`). This report covers the measured float-mode
gaps and the production multiply correction. The other mode gaps remain unresolved.

**The initial measurement found an observable signed-zero bug, independent
of the denorm warning.** `NoContraction` multiply becomes an MSL helper using
`fma(a,b,+0)`, which changes a negative-zero product into positive zero. Keeping
`SignedZeroInfNanPreserve` enabled or disabling global fast math does not fix it.
The production correction below uses a same-sign-zero FMA addend. Removing
`NoContraction` globally is wrong: the test then fuses a separate multiply/add
and changes its answer.

## Production multiply repair

The Apple profile now enables `repair_mul_signed_zero`. `EmitFPMul32` emits
`fma(a,b,bitcast_float((bits(a)^bits(b)) & 0x80000000))`, retaining
`NoContraction`. Actual guest `EmitFPFma32` is unchanged: a guest FMA with an
explicit positive-zero addend has different zero-sign semantics from multiply.
The flag defaults off, so Linux/Windows keep their previous emitted operations.
The Apple shader-binary cache version is bumped to invalidate prior SPIR-V.

Run the production acceptance with a completed shared GPU build:

```sh
BB_GPU_BUILD=/path/to/completed/out/gpu BB_FLOAT_BENCHMARK=1 \
    sh tests/macos_float_modes/production.sh
```

This compiles the actual modified floating-point emitter and a small generator,
linking the existing GPU library for `EmitContext` and the other production
emitters; it does not build a second GPU tree. It validates all generated SPIR-V,
executes the original 50 cases plus 10 overflow/Inf/NaN/FMA-zero cases, and compares
raw bits (NaNs by classification). The repaired production shader exits **0**;
its disabled-flag counterpart reproduces the two original zero-sign failures.
Separate multiply/add still returns zero, fused cancellation returns -2^-46, and
`fma(-0,1,+0)` still returns positive zero. This acceptance addresses multiply;
it does not turn the other float-mode research gaps into supported features.

The benchmark uses 65,536 invocations with 128 dependent runtime-operand
multiplies each (8,388,608 multiplies per dispatch). All 65,536 results match a
strict CPU multiply chain bit-for-bit. Six sequential runs use baseline/repaired/
repaired/baseline/baseline/repaired ordering, discard four warmups each, and
collect 60 GPU timestamp samples per variant. The measured baseline median was
0.064979 ms (IQR 0.063927–0.065490); repaired median 0.036916 ms
(IQR 0.036375–0.037333), ratio 0.5681. This is a synthetic workload result, not a
game FPS claim. SPIR-V contains exactly one XOR and one AND per repaired
multiply, plus representational bitcasts and one FMA. Raw timing samples are in
[benchmark-m4-27.2.tsv](../../tests/macos_float_modes/benchmark-m4-27.2.tsv).
The original strict multiply invokes SPIRV-Cross's `optnone` helper; avoiding
that helper is a possible reason for the measured improvement, not a separately proven cost attribution.

## GCN2 contract and the Linux comparison

The low MODE byte has two-bit fields: bits 1:0 select f32 rounding, 3:2 select
double rounding, 5:4 select f32 denorm handling, and 7:6 select double denorm
handling. Round encodings are 0 nearest-even, 1 toward +infinity, 2 toward
-infinity, 3 toward zero. Denorm encodings are 0 flush both inputs/results,
1 preserve inputs/flush results, 2 flush inputs/preserve results, 3 preserve both.
Flushing preserves the sign. Initial settings come from the program resource
register; instructions can change MODE. GCN2 names the upper fields double;
the recompiler's `16_64` naming also serves later half-precision operations.
See [AMD's Sea Islands ISA, §§3.5 and 6.4](https://docs.amd.com/v/u/en-US/sea-islands-instruction-set-architecture_0)
and [LLVM's mode encoding tables](https://releases.llvm.org/18.1.0/docs/AMDGPUUsage.html#floating-point-rounding-mode-enumeration-values).

MODE is not a blanket transformation on every register containing float bits:

- Normal f32 add/multiply/FMA are mode-sensitive. Loads, stores, moves, bitcasts,
  and sign-bit operations must not indiscriminately flush a payload.
- `V_MAD_F32`/`V_MAC_F32` have distinct, non-fused behavior and do not support
  denormals. `V_RCP_F32`/`V_RSQ_F32` also have denorm limitations; do not equate
  them with a general correctly rounded division/square root. These distinctions
  are explicit in [LLVM's AMDGPU lowering](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/SIISelLowering.cpp).
- Pre-GFX9 `V_MIN_F32`/`V_MAX_F32` do **not** flush denormals. GFX9 and later
  differ. Applying a universal flush around min/max would therefore break the
  older guest semantics. See the [LLVM AMDGPU canonicalization review](https://lists.llvm.org/pipermail/llvm-commits/Week-of-Mon-20170710/468197.html).
- Conversion opcodes can specify their own rounding: `V_CVT_PKRTZ_F16_F32`
  explicitly rounds toward zero; integer conversion/truncation and explicit
  floor/ceil/round instructions must retain their operation-specific rules.
  Legacy multiply has special zero/NaN/Inf behavior. The Sea Islands instruction
  tables distinguish these from ordinary multiply and general conversion.

This is an opcode-family audit, not a full PS4 hardware conformance proof.
Min/max NaN and zero ties need the GCN2 instruction's IEEE/legacy rules, not a
modern RDNA min/max assumption. The older manual's f32 pseudocode uses source
selection on ties; the f64 entry separately specifies signed-zero handling.
The available [AMD-authored Sea Islands manual reproduction, §§12.6–12.8](https://fr.scribd.com/document/313116019/AMD-Sea-Islands-Instrucdddd-d-Sation-Set-Architecture)
was useful for these per-op distinctions; the official PDF download was unavailable
during this investigation.

The Linux emitter relies on a Vulkan driver implementing its advertised float
execution modes. RDNA retains programmable rounding/denorm fields, but is not a
bit-for-bit oracle for every GCN2 opcode. This investigation has **not run the
probe on an RDNA GPU**. Statements about RDNA here are ISA/compiler comparisons,
not measured Linux results. See [AMD RDNA ISA §6.4](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna-shader-instruction-set-architecture.pdf).

## What this recompiler requests

`video_core/amdgpu/regs_shader.h` decodes the four fields in graphics/compute
program settings. `vk_pipeline_cache.cpp` copies them into `RuntimeInfo::props`
and fills `Profile` capabilities from `VkPhysicalDeviceVulkan12Properties`.

In `shader_recompiler/backend/spirv/emit_spirv.cpp`:

- f32 `InOutFlush` requests `DenormFlushToZero 32` only when advertised; otherwise
  it warns once and omits the mode. All other f32 denorm modes currently warn as
  unknown, including preserve-both despite the existing profile preserve flag.
- For used f16/f64, preserve-both or flush-both is selected when advertised;
  asymmetric modes and incompatible independence settings only warn. The denorm
  branch examines both widths after either width is used; rounding/preservation
  handling is more selective. This merits a capability audit for unused widths.
- RTZ emits `RoundingModeRTZ` if supported. Nearest-even emits no explicit mode.
  Directed rounding warns. Independence is reduced to a boolean (`!= None`).
- Signed-zero/Inf/NaN preservation is requested for f32 unconditionally, even if
  an otherwise integer-only shader never uses it; missing capability warns.
  Used f16/f64 receive analogous requests.

`emit_spirv_floating_point.cpp` decorates add/sub/mul/div/FMA with `NoContraction`;
min/max use GLSL extended `FMin`/`FMax`. `V_MAD_F32` is lowered to separate
multiply and add. `V_MUL_LEGACY_F32` already has a zero-result select.
`V_CVT_PKRTZ_F16_F32` currently reaches `OpPackHalf2x16` without an explicit RTZ
operation, another conversion issue requiring owner review.

## Advertised capabilities versus measured instructions

The standalone query and a separate Vulkan-1.2-properties query agree:

| Width | Denorm preserve | Denorm flush | Signed zero / Inf / NaN | RTE | RTZ |
| --- | --- | --- | --- | --- | --- |
| 16 | false | false | true | true | false |
| 32 | false | false | true | true | false |
| 64 | false | false | false | false | false |

Both independence enums are `2` (`None`). Float64 arithmetic is unsupported.
The actual game log examined has the f32 denorm warning, **not** a signed-zero
capability warning. Default test profiles with unset support flags can generate
the latter. Capability true permits the corresponding SPIR-V mode; it does not
excuse an incorrect compiler transformation. See [Vulkan float controls](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFloatControlsProperties.html).

Metal permits denormal flushing and has restricted numeric guarantees. Fast math
can additionally assume finite inputs; turning it off does not promise denorm
preservation. See [Metal language specification §§1.6.3 and 8.1–8.5](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).
MoltenVK defaults fast-math configuration to 2, allowing shader controls to
restrict it; value 0 globally disables it. Both configurations were measured,
not used as a production fallback. See [MoltenVK 1.4.2 configuration](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/Docs/MoltenVK_Configuration_Parameters.md#mvk_config_fast_math_enabled).

`tests/macos_float_modes/run.sh` feeds raw runtime buffer bits into the original 50 compute
cases plus 10 supplemental edge cases, preventing constant folding, reads raw integer results after explicit
barriers and queue completion, and runs four sequential variants. Unsupported
DenormPreserve/Flush modes are never requested. This is not a full shader or
floating-point conformance suite. Exit **2 is expected today**: it reports known
semantic differences instead of giving them a green pass. A compile/device/run
failure has a different exit status. A 45-second process alarm bounds hangs.

| Operation / input bits | Observed output | Meaning |
| --- | --- | --- |
| `1 + 1` (smallest f32 subnormals) | `00000000` | Arithmetic flushes inputs |
| `007fffff + 00000001` | `00000000` | Input flush prevents a normal result |
| `00000001 * 7e800000` | `00000000` | Preserving input would instead yield `34000000` |
| `00800000 * 3f000000` | `00000000` | Output flush; preserve would give `00400000` |
| `80800000 * 3f000000` | `80000000` | This underflow retains its negative sign |
| `00800000 - 007fffff` | `00800000` | Consistent with input flush |
| FMA input/output subnormal cases | `00000000` | Both flush |
| `min(00000001,00800000)` | `00000001` | Min preserves subnormal payload |
| `max(80000001,80800000)` | `80000001` | Max preserves subnormal payload |
| Ordered comparison ±smallest subnormal vs zero | equality | Compare treats these as zero |
| `+0 * -1`, `-0 * +1`, with NoContraction | `00000000` | **Wrong product sign** |
| `-0 + -0`, `fma(-0,1,-0)` | `80000000` | Sign preserved in these cases |
| min/max of opposite zeros, either order | second operand bits | Must compare with exact guest tie semantics |
| `1 / -0` | `ff800000` | Negative infinity preserved |
| min/max with one quiet NaN | numeric operand | Tested quiet NaNs only |
| `0 * Inf`, `Inf + -Inf` | `7fc00000` | Quiet NaN; payload/sign not guaranteed |
| f32→integer subnormal / 1.9 | 0 / 1 | Truncating conversion |
| integer 16777219→f32 | `4b800002` | Nearest-even on this tie |
| half pack/unpack smallest half subnormal | `0001` / `33800000` | Half denormal survives these conversions |
| half pack/unpack negative zero | `8000` / `80000000` | Sign survives |
| half pack `3f803000` / `bf803000` | `3c02` / `bc02` | Nearest-even; explicit PKRTZ requires `3c01` / `bc01` |
| fused `(1+2^-23)*(1-2^-23)-1` | `a8800000` | Correct fused result, -2^-46 |
| separate multiply/add, NoContraction | `00000000` | Correct non-fused result |
| same separate expression, NoContraction stripped | `a8800000` | **Unwanted fusion** |

The default, globally-safe-math, and explicit `SignedZeroInfNanPreserve 32`
variants agree on these results. The fourth, contraction-enabled diagnostic
fixes the two zero products but breaks the non-fused result. Generated SPIR-V is
validated. The probe's signature uses NoContraction as the real emitter does.
The raw 50-case results are recorded in
[results-m4-27.2.tsv](../../tests/macos_float_modes/results-m4-27.2.tsv).

The dumped Metal source contains `spvFMul(l,r) = fma(l,r,T(0))`, confirming the
zero-sign cause. The corresponding [SPIRV-Cross source](https://github.com/KhronosGroup/SPIRV-Cross/blob/main/spirv_msl.cpp)
emits that helper for legacy NoContraction/invariant float math. This is a
translation problem, not evidence that the M4 cannot represent negative zero.

## Correct-or-loud options and cost

1. **Correct strict multiply's zero sign.** The implemented Apple correction is
   `fma(a,b,bitcast_float((bits(a)^bits(b)) & 0x80000000))`. The addend is a zero
   with the product's sign, so it leaves a nonzero exact product unchanged and
   preserves the intended sign when both terms are zero. This retains one fused
   operation for a single multiply and does not fuse it with a later guest add.
   The shader test checks positive/negative zero, negative denormal input,
   negative underflow, and zero×infinity. All pass. It adds approximately an
   integer XOR and mask per affected multiply; bitcasts are representation
   changes. This is an IR-level cost estimate, not a GPU timing claim. Broader
   opcode/mode conformance remains separate from this multiply correction; the
   production follow-up above also covers finite chains, NaNs and overflow.
2. **Implement guest modes per opcode.** For the common input/output-flush mode,
   integer exponent/mantissa classification and a signed-zero select can enforce
   flush at the operations that require it. Do not flush loads or pre-GFX9 min/max
   indiscriminately. Eliminating redundant checks using value analysis is needed
   before imposing several ALU operations on every arithmetic instruction.
   A shader-wide Vulkan flush execution mode is unavailable here and also too
   coarse for guest opcode exceptions.
3. **Preserving modes need real emulation or rejection.** Wrapping a result in a
   flush test cannot recover a subnormal already lost in hardware. Exact
   preserve/asymmetric behavior requires qualified scaling/integer arithmetic or
   software floating point, including rounding, cancellation and FMA. Until
   implemented, reject affected shaders with hash, stage, MODE and opcode. The
   same applies to directed rounding and actual f64 use; do not lower f64 to f32
   before checking the requirement.
4. **Give explicit conversions their required rounding.** PKRTZ packing should
   use an exact integer conversion or a genuinely supported per-operation RTZ
   path. Changing the entire shader to RTZ would alter other operations. This
   issue exists in shared lowering and should be qualified on Linux too.
5. **Reject or count failures for uncovered semantics.** Capability checks and
   finite test samples do not justify a warning-and-continue path. Conversely,
   unused precision/mode requirements should not reject an integer-only shader.
   Keep any eventual acceptance based on actual emitted operations, required
   modes and tested implementation, not an OS-specific warning suppression.

A flushed f32 result has absolute magnitude below about 1.1755e-38, but the
observable difference is not necessarily small: comparisons can branch
differently and reciprocal/sign-sensitive operations can yield opposite
infinities. The tested no-contract sign error has zero absolute numeric error
at its output but changes its raw bits and downstream `1/x`. The non-fused test
differs by 2^-46. Half packing differs by one half-precision ULP. These are
semantic counterexamples, **not proof of a visible Bloodborne artifact**.

## Game mode inventory

Astra-1 captured all four initial fields at the program lookup site, including
warm-cache hits, in the private `play-real-bounds.log` experiment. All eight
unique hash/stage pairs captured before the pre-title compilation stop use
`denorm32=0, denorm16_64=3, round32=0, round16_64=0` (initial low MODE byte
`0xc0`): f32 flush input/output, f16/f64 preserve input/output, nearest-even.

| Shader hash | Stage | Available final guest IR float operations |
| --- | --- | --- |
| `306ce628` | Compute | None in the examined final IR |
| `83450e2f` | Compute | Unsigned↔f32 conversion, reciprocal, multiply |
| `e2c27f0c` | Compute | Unsigned↔f32 conversion, reciprocal, multiply |
| `8b355b5a` | Compute | None in the examined final IR |
| `a224d59e` | Compute | None in the examined final IR |
| `38d65b32` | Fragment | Floor |
| `89cda9e4` | Vertex | Signed integer→f32 conversion |
| `9837d1c8` | Fragment | Compilation stops at unrelated flat-buffer load assertion |

The seven successfully emitted representative SPIR-V modules all explicitly
request `SignedZeroInfNanPreserve 32`; none requests denorm flushing, consistent
with the capability check. None has float64 types or `PackHalf2x16`. Additional
backend coordinate-conversion multiplies appear in fragment/vertex SPIR-V, so
counting SPIR-V multiplies alone does not identify guest multiply instructions.
The examined compute multiplies consume reciprocal/unsigned-conversion values;
this sample does **not** prove that negative zero reaches them in game execution.

This capture uses the real buffer bounds checks but still the private dense-arena
and vertex-fetch-gate experiment. It stopped before title rendering. It is not a
complete title/movie/gameplay shader inventory and is not production acceptance.
The two preview-related hashes were also seen in earlier character-creation
experiments, but that does not expand this capture's MODE coverage. Dynamic MODE
changes would require a separate instruction audit; these are initial settings.

No proprietary shader bytes or game assets are included in this report or tests.
