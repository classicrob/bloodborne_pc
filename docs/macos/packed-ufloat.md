# Packed unsigned-float buffer conversions

The typed-buffer packer treated the IEEE-754 exponent byte as an unbiased signed
exponent. For example, `1.0f` supplied 127 instead of 0 to the conversion, producing
`0x701c0380` instead of `0x781e03c0` for three ones in `10_11_11`.

The replacement packs unsigned e5m6/e5m5 using integer bits, with exponent bias 15.
It rounds ties to even, retains denormal results, clamps finite overflow to the
maximum finite code, preserves positive infinity, clamps negative finite values
(including negative zero and negative infinity) to zero, and maps either-sign NaN
to a positive NaN. All shifts have bounded counts, including values evaluated on
unselected `OpSelect` arms; no float-to-integer conversion sees exceptional values.
The widening helper builds normal f32 encodings directly and scales small-float
denormals exactly. Its old speculative negative/wrapped shift counts are gone.
Both inverse pack/unpack folds are disabled: unpacking a packed float quantizes it,
and repacking an unpacked NaN may canonicalize its payload.

## Evidence limits

Sea Islands Table 8.13 names the formats but does not specify rounding, saturation,
denormal flushing or NaN payload behavior. The policy above follows the
[Mesa helper cited by the old implementation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/util/format_r11g11b10f.h)
and [EXT_packed_float](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_packed_float.txt),
not a new PS4 hardware measurement. The verifier reports differing edge policies as
named readings. Exactly representable normals and exact widening have no rounding
ambiguity. Do not describe an ambiguous edge-case pass as PS4 equivalence.

The canonical buffer layout is X in bits 0..10, Y in 11..21, Z in 22..31.
`11_11_10` currently shares this conversion via the existing format/swizzle remap;
this unit does not change its previously documented `PACK_MIRROR` layout reading
(see `buffer-format-conversions.md` and the conformance `SPEC.md`). Both MUBUF and
MTBUF lower through these helpers. The verifier exercises both formats and reports
which layout reading matches, rather than inferring an ordering from this fix.

Image writes use `OpImageWrite` to the bound Vulkan format and do not call these
software packers. Image reads likewise use the image unit. Their existing format
and swizzle mapping is unchanged; this fix cannot itself qualify all image-format
edge behavior. The renderer's independent Vulkan clear/fill check agrees on the
exact normal encodings used to identify the typed-buffer error.

The change applies on all platforms. Shader caches advance to Apple 30 / native 16.
