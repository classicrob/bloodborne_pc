# CPU command-buffer visibility on macOS

The production `edd8d1f` playtest reached character creation, then stopped after
finalising the contract with PM4 type 1, preceded by packet-length errors. The
memory-side investigation found no CPU parser path through a Vulkan import mirror.
It does not yet identify the malformed stream's producer or establish when its
bytes changed.

## Which address the CPU reads

`gnmdriver.cpp::PerformSubmit` constructs spans over the guest DCB/CCB pointers,
converting supplied byte lengths to dword lengths with `>> 2`.
`Liverpool::SubmitGfx` retains those spans in the `ProcessGraphics` coroutine.
The parser reads `dcb.data()` directly. Nested indirect buffers likewise use the
packet's guest `Address<const u32>()`; there is no runtime backing-address lookup
or import-mirror translation at these reads.

`BB_COPY_GPU_BUFFERS` defaults false. When explicitly enabled, `CopyCmdBuffers`
copies the top-level DCB/CCB into host vectors before queueing. It does not copy
nested IB targets. Draw-preparation copies also do not replace the parser's DCB
span. Thus a canary must distinguish top-level buffers and nested IBs.

## Aliases, protection and replacement

The runtime's guest mapping and the import arena's writable mirror both use
`mach_vm_remap` with `copy=FALSE`. GPU tracking's `mprotect` changes permissions;
it does not install a different backing object. Executable alias invalidation
cycles execute permission to discard Rosetta translations after backing writes;
it does not change data backing and is not part of ordinary PM4 data reads.

A new mapping at an existing virtual address is a different lifetime operation.
An independently retained Mach mirror can retain its old object after replacement.
The import lifetime hooks retire Vulkan imports before replacing backing; physical
release also unmaps every tracked guest alias of the released pages. Neither fact
pins an arbitrary queued command-buffer span against future guest reuse. The
mutation hook pauses Liverpool at a command-processing point and drains issued
GPU work; it does not assert that every queued DCB/IB has been fully parsed.
Same-address reuse, guest writes after publication, or replacement during a
suspended parser therefore remain timing/lifetime questions.

Queue publication itself takes `queue.m_access`; the consumer takes that same
mutex before resuming the task. This orders writes preceding publication by the
submitting thread. It does not establish correct producer synchronization before
submission, or prevent later writes to the same buffer. A malformed header in a
later snapshot alone cannot distinguish those cases from parser advancement errors.

The separately measured Metal hazard between overlapping imported GPU resources
must not be confused with CPU alias coherence. GPU writes still require the proper
submission/fence ordering before CPU consumption; this CPU-only test does not
validate that ordering.

## Executed CPU regression

`sh tests/test_memory_cpu_alias_macos.sh` compiles the actual Mach backend using
only the SDK, signs and verifies the x86_64 executable, and runs it under Rosetta.
On macOS 27.2, 3,000 synchronized publications of 1,024 words each passed. Writes
alternate through the guest view, backing pool and import-style writable mirror.
A consumer pthread compares all words through four views: original guest address,
a second guest physical alias, the backing pool, and the writable mirror. The
original guest view is read-only during every consumer check, with repeated
protection cycles and a cold initial backing allocation. No payload bytes differ.

This validates synchronized CPU data visibility, not unsynchronized concurrent
mutation, GPU completion, live command-buffer ownership, or the private canary.
No production memory or renderer source changed.
