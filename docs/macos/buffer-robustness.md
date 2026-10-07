# Guest buffer bounds on macOS

The Apple shader profile enables `emulate_robust_buffer_access`. Guest memory accesses use explicit SPIR-V bounds checks instead of requiring Vulkan `robustBufferAccess2`. The native Linux and Windows profiles retain their existing behavior.

The reference is *AMD Sea Islands Series Instruction Set Architecture*, Revision 1.0, February 2013, sections 7.2.2 and 8.1.4–8.1.6. The independent conformance suite records the reference interpretation and ambiguous cases in `tests/gcn_buffer_conformance/SPEC.md`. Its verifier compared these sections with Southern Islands and found the text identical. Liverpool is Sea Islands; the implementation uses those rules.

## Bounds and alignment

`BufferAddress` preserves the guest index, VGPR offset, literal offset and SGPR offset until resource patching knows the descriptor. Linear and swizzled addresses are calculated separately. The SGPR offset applies to the base after swizzling.

For raw descriptors (`stride == 0`), `num_records` is the byte bound. Structured descriptors check the record index; they additionally check the offset against the stride when index enable or add-thread-ID enable applies. Checks use the unaligned starting offset. DWORD accesses align the complete byte address, including the guest base's alignment prefix, after that check. Raw DWORD vectors check each component separately; formatted accesses check and discard the whole element.

Every access also checks the actual Vulkan binding capacity. The binding ends at the guest descriptor bound; alignment padding at its beginning does not enlarge the guest bound. Partial host elements return zero or discard the entire element, as recorded by the conformance suite's allowed interpretation of straddling accesses. No access chain for a failing load, store or atomic is formed.

Scalar `s_buffer_load` uses its distinct ISA clamp rule. A clamped destination SGPR keeps its previous value. INVALID data format does not suppress scalar loads; it suppresses MUBUF loads, stores and atomics. This is operation-specific, so one descriptor can serve both instruction classes correctly.

## Explicitly unsupported cases

These cases stop with a descriptor diagnostic rather than returning a plausible but incorrect value:

- Nonzero or dynamic SGPR offsets on structured descriptors, which need a binding wider than `GetSize()`.
- Dynamic offsets on structured accesses without index or add-thread-ID bounds.
- A constant such access whose `required_binding_bytes` exceeds the bound at the current use. The resource tracks its minimum span; the binder checks it for every use.
- Incomplete swizzle index groups and structured sizes that overflow `GetSize()`'s 32-bit representation. The binder checks these before classifying a zero size as null.
- Invalid swizzle element layouts.
- Descriptor add-thread-ID behavior with a subgroup narrower than the guest's 64-lane wave. General wave64 emulation is a separate correctness unit.

The descriptor diagnostics include shader hash, base, stride, record count and swizzle fields. The structured binder expansion is deferred until a real trace exercises a rejected case.

## Shader cache and cost

The shader specialization includes descriptor add-thread-ID enable and MUBUF INVALID-format classification. Runtime `num_records` values come from the flattened descriptor data. The emulation profile is serialized and compared, and the shader and pipeline cache versions change with the semantics.

The shader prologue reads each binding's smallest alias capacity once with `OpArrayLength`, derives the other alias capacities, and reads each guest record count once. Loads use a guarded branch and a zero-result Phi. Stores and atomics execute only within the guarded branch. There is one additional constant-span comparison in the Apple binder for structured accesses with no offset bound.

`tests/test_buffer_bounds.cpp` exercises the actual production emitters through MoltenVK: raw boundaries and alignment, raw vector and 64-bit components, structured bounds, complete swizzle groups, whole formatted elements, INVALID-format scalar/vector sharing, stores, atomics and neighboring canaries. `tests/gcn_buffer_conformance/` independently tests encoded GCN through translation, specialization reuse, SPIR-V emission and execution.

`tests/test_buffer_bounds_benchmark.cpp` measures 65,536 compute invocations with 128 data-dependent loads each, checks every output against the proven-in-range control, and uses Vulkan GPU timestamps. On M4 Max/macOS 27.2 under Rosetta, an initial run measured 0.053 ms without guards and 0.111 ms with guards, averaged over ten samples after two warmups. The additional 0.058 ms is a synthetic guard cost; it is not a gameplay or 4K frame-time measurement. Further optimizations must pass the same conformance tests first.
