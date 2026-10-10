# Packed half conversion with explicit RTZ

On Apple, `V_CVT_PKRTZ_F16_F32` carries an RTZ flag on `PackHalf2x16`.
The SPIR-V emitter converts both binary32 bit patterns with integer operations
and packs the low/source-0 and high/source-1 halves. Ordinary half packing and
Linux/Windows translation keep their existing path. Apple shader cache version
21 invalidates previously rounded SPIR-V.

Finite results truncate toward zero: normal significands discard low bits,
subnormal outputs shift the implicit leading bit, underflow produces signed
zero, and finite overflow saturates to signed maximum finite half. Input
binary32 subnormals always underflow to half zero, independent of input-denorm
mode. Half output denorms follow the same compiler-derived MODE policy as the
qualified native F16 conversions: modes 0/1 flush output, modes 2/3 preserve it.
Infinity remains infinity; NaNs remain quiet NaNs. Tests check NaN classification
rather than asserting an unqualified payload rule.

Sea Islands ISA Rev. 1.0 (February 2013), VOP2 opcode 47 / VOP3 opcode 303,
explicitly requires round toward zero. It does **not** specify half-output
denorm handling or NaN payloads. Its MODE table names the upper field double
precision. The output-denorm interpretation is compiler evidence, not a
physical PS4 measurement: Mesa ACO's `alu_default_needs` retains source FP32 and
destination FP16/64 denorm requirements for PKRTZ, while
`opcode_ignores_round_mode` removes rounding-mode dependencies for that opcode.
See [AMD's ISA catalog](https://gpuopen.com/amd-isa-documentation/) and
[Mesa's mode pass](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/amd/compiler/aco_insert_fp_mode.cpp).

The pack/unpack inverse-fold optimization is disabled for the flagged operation
in both directions. An unpacked finite value can lose precision when repacked;
an exactly representable half subnormal can flush; a signaling NaN can quiet.
Removing the conversion would therefore change observable bits.

## Validation

Run `BB_GPU_BUILD=/path/to/stable/gpu tests/macos_pkrtz/run.sh` with matching
checkout headers. The script signs its executables and runs translated processes
sequentially. The emitter test calls the production packing function. A separate
optimizer test proves both RTZ pack/unpack round trips survive constant propagation.

On M4 Max, macOS 27.2, MoltenVK 1.4.2:

- 63,510 runtime-fed packed pairs per mode, four modes: **254,040 pairs PASS**.
- Every finite positive half boundary is tested at the exact value and just
  below the next boundary, in both signs and both packed components. Additional
  cases cover signed zero, binary32 subnormals, half underflow, overflow,
  infinities and signaling/quiet NaNs.
- The independent C reference searches the ordered half-value table for the
  greatest representable magnitude not exceeding the input, rather than using
  the emitter's exponent/shift algorithm.
- Input and tail canaries are intact; all four modules pass `spirv-val`.
- An ordinary RTE packing negative control fails **63,501 / 63,510 pairs** in
  preserve mode, proving the probe detects the original rounding gap.

The new path adds two branchless integer classification/conversion chains per
executed PKRTZ, with no per-draw CPU work and no Float16/RTZ capability requirement.
GPU timing is not yet qualified; this correctness unit makes no performance or
game-frame-rate claim.
