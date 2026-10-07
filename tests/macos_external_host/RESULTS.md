# Mach backing imported into MoltenVK

Measured on macOS 27.2 (26B5101f), M4 Max, Rosetta x86_64, shared MoltenVK 1.4.2.
Run `bash tests/macos_external_host/run.sh`. It builds only a standalone C probe,
the actual included `runtime_memory.c`, and one tiny GLSL compute shader. No GPU
library rebuild. `glslangValidator` runs natively from Homebrew.

| Property / test | Result |
| --- | --- |
| Imported host pointer alignment | 4096 bytes |
| Tested buffer memory requirement alignment | 256 bytes |
| Host pointer memory types | bit 1; selected type 1, flags `0xf`: device-local, host-visible, host-coherent, host-cached |
| External buffer features | IMPORTABLE; compatible handle mask `0x180` |
| Reported maximum buffer and memory allocation | Both 86,586,540,032 bytes (~80.64 GiB); the maximum itself was **not** allocated |
| Actual import sizes | 4 KiB, 32 KiB, 64 KiB, 1 MiB, 16 MiB, 256 MiB, 4 GiB, 8 GiB all pass |
| Device address | Nonzero, validated by actual shader dereference; address need not equal host/guest address |
| Large range endpoint | Shader reads and writes the last two words of both 4/8-GiB imports successfully |
| Actual Mach backing and guest aliases at 448 GiB | Both import, both-way CPU/GPU coherence across two aliases and backing passes |
| Two simultaneous imports of the same physical payload through different aliases | Both work, writes visible through both aliases |
| Noncontiguous backing | Two adjacent 16-KiB guest maps from physical offsets 0 and 32 KiB import as one 32-KiB range; GPU writes reach the correct separated backing pages |
| Guest alias protected RO | GPU writes still work, including when that alias is the imported pointer; CPU write faults and handler restores RW/resumes |
| Already-RO cold view / overlapping imports | Dedicated RW Mach alias per import works: 64-KiB left/right and encompassing 192-KiB imports simultaneously, cross-import shader reads/writes coherent; original guest remains RO and still faults on CPU writes |
| Backend-triggered retirement | Outstanding shader submission is waited in begin; both alias imports and their RW mirrors are destroyed while old bytes remain readable; end observes flushed notifications, then fresh-zero reimport works |
| Release/zero/reuse | After waiting and destroying imports, actual direct release removes aliases and replaces backing; reallocation reads zero, reimport has working BDA |

The compute test reads a CPU-written word through a buffer-reference device address
and writes the sum to the previous word. It uses HOST->COMPUTE and COMPUTE->HOST
barriers and queue completion before inspecting CPU memory. No staging copy or
explicit mapped-memory flush/invalidate was used with the tested coherent type.
This tests ordered access, not concurrent unsynchronized CPU/GPU writes.

Indicative one-shot create/query/import/bind/BDA setup timings: 256 MiB 8.99 ms,
4 GiB 141.57 ms, 8 GiB 313.55 ms. They are **not per-frame costs**, sustained GPU
bandwidth, or production performance measurements. Only endpoints were touched;
getrusage reported peak RSS 13.3 MiB. That counter does not establish total driver
or GPU residency. Import setup is large enough that a production cache must retain
imports rather than recreate them each draw/frame. No large disk outputs.

## Lifetime and tracker implications

[Vulkan host-import lifetime rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html)
require the imported host range to remain valid and accessible for the import's
lifetime. The probe deliberately does **not** replace/unmap backing underneath a
live import and then interpret undefined behavior as a guarantee. Correct sequence:
retire all commands using the range, destroy affected buffers/imported memory,
change mappings or replace backing, then reimport and republish BDA before reuse.
An identical BDA returned later is not evidence that the old object stayed valid.

The runtime's previous direct-release sequence replaced backing before flushing
queued GPU-unmap notifications. Import integration needs an earlier synchronous
retirement phase. This also applies to flexible frees, fixed-address overwrite,
and batch mapping operations, not just direct release. Imports spanning multiple
VMAs must be retired if any constituent range changes. All guest aliases of released
physical storage are affected.

GPU writes bypass CPU `mprotect` write faults, as expected from separate GPU mappings.
The existing shadow-copy tracker therefore cannot infer GPU writes from those faults.
It must retain GPU command dependency/dirty tracking while avoiding upload/download
copies onto the same physical storage. Importing the guest view also requires
retirement before its virtual address is removed even if some backing alias survives.
The chosen production renderer implementation and its lifetime protocol belong to
Sol-3; this probe changes no arena implementation.

## Import-time protection qualification

Importing a guest view that is already read-only produced zero instead of the GPU
writes in the cold 192-KiB overlapping-import test. Pre-touching the entire backing
through a writable CPU alias did not fix it. Importing while writable and changing
the guest view to read-only afterward did work. This narrows the earlier RO result:
it tested protection changes after import, not import of an already-RO view.

The tested production-compatible path gives each import a dedicated writable Mach
alias. `mach_vm_remap(copy=FALSE, VM_FLAGS_ANYWHERE, VM_INHERIT_NONE)` of an already-RO
guest range returned current protection `1`, maximum `7`; protecting **only the
new alias** RW succeeded. Vulkan imports that alias. Guest CPU permissions never
change during setup, so concurrent CPU writes cannot escape tracking. No prefault
or data copy is needed. The import stays indexed by its original guest-view range.
After GPU retirement, destroy the Vulkan buffer and memory before deallocating the
Mach alias. The backend hooks invalidate every affected original view before a
mapping or physical backing change.

This passes cold zero backing, three simultaneous overlapping imports, a shader
reading another import's shader output, and a single alias spanning two guest VMAs
with noncontiguous physical offsets. A subsequent CPU write still delivers the
expected protection fault. The queued-work lifetime test now uses these writable
mirrors too. Alias setup adds one Mach remap/protect per retained import and a
virtual alias; it adds no per-draw copies. Full renderer pause/retirement integration
remains a separate test from this standalone callback fixture.
