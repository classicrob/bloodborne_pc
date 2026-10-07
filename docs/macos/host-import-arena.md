# macOS guest buffer imports

Apple guest buffers use non-sparse `VK_EXT_external_memory_host` allocations. Each import owns a writable `mach_vm_remap(copy=FALSE)` mirror of a complete mapped guest VMA, preserving the original guest protection. Vulkan imports the mirror with coherent, host-visible, device-local memory and a real buffer device address. The buffer cache publishes that address plus the guest offset in its existing 16 KiB BDA page table.

Importing an already read-only guest pointer directly is incorrect on the tested MoltenVK 1.4.2 / M4 Max / macOS 27.2 configuration: GPU writes do not reach the guest. A dedicated writable mirror preserves shared bytes without temporarily disabling CPU write tracking. No guest-buffer upload or download copies are performed. Image tiling conversions remain necessary and retain their existing operations.

A resource covering the current VMA is created on first use. Ordinary reuse needs one ordered-map lookup; backing queries and Mach/Vulkan allocation happen only when creating an import. Astra-2 measured a 4 GiB import at about 142 ms and an 8 GiB import at about 314 ms, without bulk prefaulting. These are setup costs rather than frame costs.

## Growth synchronization

The production allocation and arena-map fixture measured a MoltenVK alias hazard: GPU writes through two imports followed by reads through an overlapping replacement import in the same submission returned zero, despite an ALL_COMMANDS memory barrier. Reading through the original imports in that submission returned the correct values. Waiting for completion before replacement use returned the correct values through the replacement, including when the replacement had been created before submission.

The arena map therefore requires GPU completion before creating and rebinding an overlapping replacement. Each completion wait is counted and logged with the guest range, frame number and elapsed microseconds. Importing the complete VMA initially avoids repeated growth as bindings advance through that allocation. Replacement updates BDA metadata and preserves shared bytes; it does not copy a shadow arena.

## Lifetime and unsupported physical aliases

The runtime's begin/retire/end transaction pauses the renderer, drains recording and submitted GPU work, and destroys affected buffers, imported memory and mirrors before replacing guest backing. Retirement occurs under the runtime writer lock and makes no VMA queries or waits. Protection state is cleared after that lock is released and map/unmap notifications have completed, before the GPU resumes.

Guest-interval overlap is insufficient to detect physical aliases. Import creation queries `runtime_memory_backing_region` for exact direct/flexible backing offsets. A distinct guest view overlapping an existing physical import, or a view that repeats physical backing within itself, aborts with both guest and backing intervals. Canonical reuse across distinct guest aliases remains unimplemented; it is not silently treated as coherent. Untracked mappings without a runtime direct/flexible backing identity also abort explicitly.

## Verification

`tests/test_host_import_arena.cpp` uses production `UniqueBuffer::ImportHost` and the production arena/backing maps. Under Rosetta on macOS 27.2 it checks cold read-only guest views, real BDA shader writes, CPU alias coherence, guest-to-BDA table offsets, growth with the completion callback, unchanged readback canaries, import retirement and fresh-zero reimport. It also checks backing-identity conflict detection, fragmented backing ranges and removal after partial retirement.

Astra-2's separate real-memory-backend fixture verifies outstanding GPU work is waited, all alias imports are destroyed before physical replacement, and a fresh import sees zero. SDK syntax checks cover the buffer cache, rasterizer lifetime bridge, shim and device extension gate. Full renderer integration remains subject to the single shared GPU rebuild and the vertex-fetch device gate.
