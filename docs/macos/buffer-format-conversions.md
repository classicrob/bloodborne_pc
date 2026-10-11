# Buffer-format conversion corrections

The skinning-format conformance matrix exercises the fetch shader's descriptor-formatted MUBUF instructions, including UINT bone indices consumed as integer VGPR bits. Ordinary 8/16-bit UINT, normalized and scaled conversions matched the reference. Two independent conversion defects were found; their effect on the character preview still requires the live descriptor capture.

For a UINT/SINT buffer, `dst_sel ONE` must produce integer bits `0x00000001`, rather than float `1.0` bits `0x3f800000`. Sea Islands ISA section 8.1.3 specifies integer results for integer formats. The swizzle helper now takes an explicit integer-result flag. Typed buffer loads derive it from the original descriptor/instruction number format, before format remapping, so USCALED/SSCALED and 32-bit UNORM conversions continue to receive float `1.0`. Fixed-function vertex fetch uses the same original-format test. Image operations and store swizzles retain their previous helper calls.

The packed `2_10_10_10` SNORM decoder divides signed components by their positive maximum, then clamps the result to at least `-1`. This maps the most-negative 2-bit component (`-2`) to `-1`, and also covers the most-negative 10-bit components. The upper endpoint is already at most `1`. This changes the shared buffer decoder on all platforms.

Both corrections apply to Apple and native typed-buffer loads; the integer swizzle also applies to the native fixed-function fetch translation. Shader caches are invalidated on both paths. The ONE correction changes a compile-time constant and adds no per-access work. Packed SNORM decoding adds one vector maximum operation per packed load, with no branches, extra buffer reads or host work.

The `10_10_10_2` and `11_11_10` component-order disagreement with the verifier's Mesa-derived reference remains documented as an ambiguity. The Sea Islands ISA lacks a bit-layout table, and this unit makes no layout change without additional hardware or authoritative layout evidence. FP16 NaN payloads and the packed UNORM reciprocal/division rounding difference are likewise outside this correction.

Regression coverage lives in the independent `tests/gcn_buffer_conformance` vertex-format group: descriptor-formatted MUBUF, packed signed/unsigned edge values, scaled and normalized controls, swizzles, real indexed draws, and out-of-range vertices. Its acceptance report must distinguish the strict integer-ONE/SNORM checks from the remaining layout ambiguities.
