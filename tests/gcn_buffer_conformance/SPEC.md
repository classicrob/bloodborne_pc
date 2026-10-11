# GCN2 buffer access: the reading this suite tests

Source: *AMD Sea Islands Series Instruction Set Architecture*, Reference Guide, Revision 1.0,
February 2013 (the PS4 "Liverpool" GPU is GCN2/Sea Islands). AMD's old URL
`developer.amd.com/wordpress/media/2013/07/AMD_Sea_Islands_Instruction_Set_Architecture.pdf`
now redirects. The copy used here came from web.archive.org. The Southern Islands manual
(x.org/docs/AMD/old/) has word-for-word the same text in every section quoted below, so GCN1
and GCN2 agree on these rules.

`ref.c` implements this file, using only the ISA text. It does not use the recompiler.

## Address calculation (§8.1.5)

    base   = const_base + sgpr_offset
    index  = (inst_idxen ? vgpr_index : 0) + (const_add_tid_enable ? thread_id[5:0] : 0)
    offset = (inst_offen ? vgpr_offset : 0) + inst_offset

Linear (§8.1.5.1): `buffer_offset = index * const_stride + offset`

Swizzled (§8.1.5.2), when `const_swizzle_en`:

    index_msb  = index / const_index_stride      index_lsb  = index % const_index_stride
    offset_msb = offset / const_element_size     offset_lsb = offset % const_element_size
    buffer_offset = (index_msb * const_stride + offset_msb * const_element_size) * const_index_stride
                    + index_lsb * const_element_size + offset_lsb

`address = base + buffer_offset` (§8.1.5.4). "For Dword or larger reads or writes, the two LSBs
of the byte-address are ignored, thus forcing Dword alignment" (§8.1.6).

## Range checking (§8.1.5.3), quoted

> When the const_stride == 0, the const_num_records is the size of the buffer in bytes.
> - If ((const_stride == 0) && (buffer_offset >= (const_num_records – sgpr_offset))
>   – If op is a write or atomic, drop the write. – If op is a read or atomic, return 0.
> - If (const_stride != 0 && ((index >= const_num_records) || ((inst_idxen | const_add_tid_enable)
>   && (offset >= const_stride)))  – (same)
>
> Load/store-format-* instructions and atomics are either completely in-range or completely
> discarded. Load/store-dword_x2 and x4 are range-checked per component.

Consequences the suite asserts strictly:
- Raw (stride 0): sgpr_offset narrows the range: the last valid start byte is
  `num_records - sgpr_offset - 1`.
- Structured (stride != 0): the check is on the **index**, never on index*stride. An index
  ≥ num_records is out of range even when index*stride wraps 32 bits to a small value
  (e.g. stride 16, index 0x10000001).
- Structured: offset < stride is checked only when idxen or add_tid is set. Structured +
  offen-only accesses are not range-checked on the offset at all, and **sgpr_offset is never
  range-checked for structured buffers**. Such accesses can legally reach memory beyond
  `stride*num_records`. The renderer binds only `GetSize()` bytes, so an exact implementation
  has to reach past the binding or fail loudly. A silent zero is a failure.
- Per component for DWORDxN (x3 is treated like x2/x4): component i is checked at
  `offset + 4*i`. Byte/short accesses are single components.
- Formatted and atomic accesses are all-or-nothing, checked at the element start.
- num_records = 0 makes everything out of range (raw and structured).
- A formatted element of 4 bytes or more (e.g. 8_8_8_8, 16_16) is a "Dword or larger" access and is
  dword-aligned like a DWORD load.
- The check runs on the unaligned `buffer_offset`. The two-LSB alignment applies afterwards, to
  the full byte address. Example: raw, num_records 8, dword at offset 6 → in range, reads bytes
  4..7.

## Data format INVALID (§8.1.4 NOTE, Table 8.5 footnote)

> The resource's data format must not be INVALID; that format has special meaning (unbound
> resource), and for that case the data format is not replaced by the instruction's implied
> data format.
> A resource set to all zeros acts as an unbound texture or buffer (return 0,0,0,0).

Suite: MUBUF (raw and BUFFER_*_FORMAT) with resource dfmt 0 → loads 0, stores and atomics dropped.
The ISA only states "unbound" and never spells out stores; the suite's reading is that no memory
changes. `S_BUFFER_LOAD_*` ignores the format (§7.2.2: "All other buffer resource fields are
ignored").

## Scalar buffer loads (§7.2.2, §7.4)

> Memory Address = Base (from V#) + OFFSET, truncated to a DWORD address.
> The address is clamped if: Stride is zero: clamp if (offset >= num_records);
> Stride is non-zero: clamp if (offset > (stride * num_records))
> If the memory address is out-of-range (clamped), the operation is not performed for any
> Dwords that are out-of-range.

## Genuine ambiguities (each is a "reading" bit in ref.h; reading 0 is the literal text)

| bit | literal reading (0) | alternative (1) | why it is ambiguous |
|---|---|---|---|
| WRAP32 | index, offset and buffer_offset are exact integers (the final address is 40/48-bit) | 32-bit wrap of every intermediate | the ISA never states their width |
| SOFF_SUM | raw: `buffer_offset >= (u32)(num_records - sgpr_offset)` | `buffer_offset + sgpr_offset >= num_records` | they differ when sgpr_offset > num_records (the literal 32-bit subtraction wraps to "huge") |
| END_WHOLE | each unit is checked at its start byte only | a unit whose touched bytes run past the end of the buffer (num_records bytes, or stride*num_records) is out of range | §8.1.5.3 shows only the start comparison; it doesn't say whether straddling bytes are masked |
| SMEM_GE | scalar, stride≠0: clamp if `offset > stride*num_records` | `>=` | `>` admits one dword past the end, which may be a typo |
| SMEM_ZERO | an out-of-range scalar dword leaves SDST unwritten | it writes 0 | §7.4 says "not performed"; drivers often describe zero |
| FMT_SEL | out-of-range format load returns 0 in every component | dst_sel constants (SEL_1) still apply to the zero data | "return 0" vs the later dst_sel stage |
| SHORT_ALIGN | 16-bit accesses use the exact byte address | the address LSB is ignored for 16-bit accesses | §8.1.6 only covers Dword or larger; misaligned shorts are not described |
| SEL1_FLOAT | dst_sel ONE is 1 in the number format (integer 1 for UINT/SINT; "integer formats are returned as integers", §8.1.3) | ONE is 1.0f for every format | DECIDED (orchestrator, 2026-10-10): integer 1; the alternative is no longer accepted |
| MTBUF_BOUND | resource dfmt INVALID makes TBUFFER_* unbound too | TBUFFER_* uses the instruction format and accesses normally | the NOTE is written for "derived" formats |
| PACK_MIRROR | packed formats (10_11_11, 11_11_10, 10_10_10_2, 2_10_10_10) laid out from the LSB, X in the low bits (Mesa's BUF_DATA_FORMAT mapping) | the same fields laid out from the MSB, X in the top bits | Table 8.13 gives no bit layout |

A result is **STRICT PASS** when every reading agrees and the implementation matches. It is
**AMBIGUOUS PASS** when it matches only some readings (each reported). Any memory effect or
return value that no reading produces is a **FAIL**. Reads and writes of canary bytes just past
the bound, and partial writes, always fail.

## Loud results (policy "correct or loud")
An abort in the recompiler or the binder, carrying a message, counts as loud. Loud is
**required** when every reading's outcome touches memory outside the bound Vulkan range.
It is **approved** for the orchestrator's interim categories: structured buffer with
nonzero/dynamic sgpr_offset; swizzled num_records % index_stride != 0; Apple incomplete
mapping. Otherwise it is reported as **LOUD (not required)**: not wrong, but it would block a
game that uses that access.

## Outside this suite
- base_address 0 with a valid format: not "unbound" per the ISA. It is a VM fault on the real
  hardware. The renderer treats it as null; the suite doesn't assert it.
- Heap bit (122), ATC, LDS-direct loads, ADDR64, TFE: not used by the recompiler.
- 64-bit atomics with return (glc): the frontend has no path for them (UNREACHABLE).

## V_CVT_PKRTZ_F16_F32 (group `alu`)
Chapter 12: `D = {flt32_to_flt16(S1.f), flt32_to_flt16(S0.f)}, with round-toward-zero`. Truncation
and overflow are therefore exact: finite values beyond the half range give the largest finite half
(0x7BFF), never infinity. The text says nothing about half-precision denormal results (Sea Islands
has no half-precision FP_DENORM field) or about NaN payloads. The suite accepts either reading for
denormal results, kept or flushed (one reading for the whole job, reported), and any NaN for a NaN
input.

## Number formats and packed layouts (groups `vertex-format`, typed loads)
Table 8.13 lists the buffer data and number formats but not the conversions or packed bit
layouts. The model uses the D3D10+/AMD conventions:
- UNORM x/(2^n-1); SNORM max(x/(2^(n-1)-1), -1) (the most negative code clamps to -1);
  SNORM_OGL (2x+1)/(2^n-1); USCALED/SSCALED float(x); UINT/SINT the zero/sign-extended integer;
  FLOAT 32-bit as is, 16-bit halves and the unsigned 11/10-bit floats widened exactly.
- Packed formats are named MSB first, X in the low bits of the name's last field:
  2_10_10_10 has X in bits 9:0 and W in 31:30 (Mesa maps R10G10B10A2 to BUF_DATA_FORMAT_2_10_10_10);
  10_10_10_2 has X (2 bits) in 1:0 and W in 31:22; 10_11_11 has X (11 bits) in 10:0, Z (10) in 31:22;
  11_11_10 has X (10 bits) in 9:0, Z (11) in 31:21. Reading PACK_MIRROR: the same fields laid
  out from the MSB (X in the top bits: 10_10_10_2 X in 31:30, 11_11_10 X in 31:22). No ISA text or
  hardware data decides it; both are accepted and the report names the one matched.
- Not fixed by the ISA, accepted as PASS(ambiguous) with the reason named: NaN payloads of FLOAT
  formats (any NaN), and the last bit of UNORM/SNORM conversions (division vs a reciprocal
  multiply: within 1 ulp, same sign).
- dst_sel ONE: integer 1 for UINT/SINT (§8.1.3), 1.0f for every other number format (decided;
  1.0f for an integer format FAILs: a bone index swizzled to ONE would read 0x3F800000).
