# Native arm64 macOS with embedded FEXCore: feasibility

2026-10-11. Source study only, using `gh api`; no builds or execution. **Go for a
bounded CPU-only prototype; no-go for replacing the Rosetta build yet.** The useful
change is that our runtime, renderer, shader compiler and MoltenVK would execute
as native arm64, while only the eboot and linked x86 PRXs remain translated.
GPU shaders already execute on the GPU; this does not accelerate them by itself.

## What the Linux fork actually embeds

Reviewed [commit 3d56243, “aarch64: FEXCore guest CPU”][integration], its diff and
the FEX submodule pinned at **9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf**. The
integration is a small embedding layer, not a Linux guest process or full FEX
syscall emulation environment:

- `cpu/bbcpu.cmake` builds `libbbcpu.so`, statically linking FEXCore, Common and
  allocator dependencies. FEX is the top-level CMake project; its normal library
  thunks and Linux tests are disabled. The rest of bbport builds for the host.
- [`guest_cpu_fex.cpp`][embedding] creates one Context and a FEX CPU state per host
  thread. Module initializers, eboot entry, thread entries and nested callbacks
  use `Context::HandleCallback`. Guest stacks are separate, low-address mappings;
  nested callbacks use space below the guest red zone. Thread exit releases FEX
  state; the runtime's nonlocal exit calls `guest_cpu_abandon`.
- Imported functions become low-address x86 stubs (`mov r10,rcx; syscall; ret`).
  A custom `SyscallHandler` recognizes their addresses and invokes native HLE;
  unrelated guest syscalls stop. This `syscall` is decoded by FEX, not executed
  as a host kernel syscall. Data imports remain data pointers.
- The [bridge][bridge] maps six SysV integer registers, additional stack slots
  and XMM arguments to Linux AAPCS64 registers/stack, then copies integer/FP
  results back. Its documented contract excludes structures by value and
  `va_list`. Host-to-guest `GuestCall` supports integer/pointer callbacks, including
  AvPlayer allocators/events, TLS destructors and finalizers. It is not a general
  arbitrary-signature ABI bridge.
- `guest_cpu_set_gs` sets **both emulated FS and GS bases** to the guest TCB.
  Native pthread TLS stays independent. Guest `RDTSC` and HLE clocks use the same
  scaled ARM counter. No hardware FS/GS register switch is needed.
- Fault handling first recognizes FEX call-return-stack guards, JIT overflow and
  unaligned atomic faults; then bbport handles protected GPU memory. Guest RIP
  is reconstructed from JIT PC, with register-allocation metadata restoring
  guest GPRs. The code reads Linux `ucontext`, FPSIMD/ESR records and uses
  `process_vm_readv` for safe reads.

The [included fixture][fixture] exercises integer/FP crossings, stack arguments,
nested callbacks, GS and TSC. It does not establish macOS compatibility or a
Rosetta performance comparison. The commit also includes unrelated shader/SRT
and GPU changes: transplant the CPU boundary selectively, not the whole commit.

## What must change for macOS

| Area | Required work / qualification |
| --- | --- |
| FEX platform support | The pinned [CMake platform check][fex-cmake] rejects Darwin. Host-feature detection, allocator hooks, `/proc/self/maps`, `prctl`, `MAP_FIXED_NOREPLACE`, ELF assembly directives and library/build assumptions need Darwin implementations. This is a FEXCore port, not merely a new compiler flag. |
| JIT memory | The pinned [code-buffer manager][code-buffers] assumes writable executable mappings. Use a managed `MAP_JIT` arena, hardened-runtime `allow-jit` entitlement as applicable, and per-thread `pthread_jit_write_protect_np` write/execution transitions (or Apple's callback API). Cover code generation, linking, invalidation and unaligned-access patching; restore execution permission before callbacks/resuming JIT, including signal recovery. Flush the instruction cache. Guest x86 bytes are data to the native CPU and do not themselves require host execute permission. [Apple JIT rules][apple-jit] |
| Native ABI | Darwin arm64 differs from Linux AAPCS64 in stack argument packing, variadics and narrow-argument extension; x18 is reserved. Audit FEX-generated helper calls as well as `host_call_arm64.S`. Use typed/generated import adapters for signatures the simple register copier cannot express; reject unsupported signatures. Preserve guest data layouts, x87/long-double semantics, aggregate returns and callback reentrancy explicitly. [Apple ABI][apple-abi] |
| Signals and tracking | Replace Linux signal-frame/FPSIMD/ESR access with Darwin arm64 context handling and Mach-safe reads. Requalify SIGBUS/SIGSEGV write tracking, FEX guard recovery and unaligned atomics together. A Mach exception debugger is not required, but Darwin exception-to-signal behavior must be tested. Existing x86 INT3 probes, profiler load-recovery PCs and HLE trace assembly cannot be reused unchanged. |
| Address space / pages | Keep all guest-visible pointers below **1 TiB (40 bits)**. Our current low arena is `0x0800000000..0x0fc0000000`; Apple guest mappings begin at `0x7000000000`, avoiding Rosetta's guard. Native execution removes that Rosetta constraint, but initially retain the layout. FEX's 47/48-bit allocator policy does **not** enforce the game's 40-bit limit. Separate JIT/host-only allocations from guest stacks, TLS, callback data and import stubs; never assume a native host stack pointer is guest-safe. Port Linux reservations to our Mach backend and qualify 16 KiB host pages, especially FEX guard-page alignment/protection. |
| GPU integration | Rebuild all dependencies for arm64 and retain the qualified Vulkan/MoltenVK path. The Mach backing/import arenas remain useful. Native execution does not fix labels, flip retirement or CPU/GPU resource lifetime; those invariants must remain intact. |

## TSO and likely performance

Treat hardware TSO as **unavailable through a supported public macOS API** for
this prototype. Rosetta benefits from Apple's hardware x86 ordering support;
the native FEX path must emulate ordering in generated code. FEX documents
acquire/release/barrier costs, unaligned atomics and particularly expensive
vector/memory-copy ordering. Its historical defaults relaxed some of those
operations for performance. Do not inherit configuration files or turn ordering
off to claim a win: pin and report scalar, vector, string-operation and atomic
semantics, with contention/litmus tests. Native HLE's shared-memory accesses also
need an ordering audit. [FEX ordering discussion][fex-tso], [default-policy change][fex-defaults]

There is **no measured speedup estimate** for this game on native macOS. For a
fixed amount of serial CPU work, a useful sensitivity model is
`T_native/T_rosetta = g*f + (1-g)/s + c`, where `g` is guest CPU share, `f` is
FEX guest slowdown, `s` is native host-code speedup and `c` is added crossing/JIT
cost relative to the old CPU time. Illustratively, assuming `f=s=1.5`, `c=0`:
25% guest gives 0.875 times the CPU time (about 14% faster); 50% guest gives
1.083 times (about 8% slower). These are scenarios, not predictions. Parallel
stages, GPU time and waits require a critical-path comparison; our current
wait-heavy frame times cannot be converted into an FPS gain with this formula.

## Bounded prototype and decision gates

Planning estimate for one experienced engineer, not a delivery commitment:

1. **3–5 days: platform spike.** Inventory Darwin blockers and establish a native
   FEX code-cache/allocator path. Stop and reassess if this requires a broad fork
   of FEX's core rather than contained platform adapters.
2. **Another 2–4 weeks: CPU-only qualification.** Execute tiny x86 fixtures;
   qualify typed HLE calls/returns, nested callbacks, FS/GS, thread exit, TSC,
   16 KiB guards, protected-memory recovery, code invalidation and strict ordering.
   Measure guest arithmetic, memory/atomics and hot HLE crossings against Rosetta.
3. **Another 4–8+ weeks: game integration and performance qualification.** Build
   native dependencies, restore diagnostics, run save-fixture gameplay and compare
   identical warmed scenes. JIT ABI/TSO/fault discoveries can extend this estimate.

Keep Rosetta as the supported path throughout. Proceed beyond the CPU prototype
only if strict correctness passes and the measured guest cost leaves room for
native renderer/runtime savings. The source offers a credible architecture to
reuse; it does not establish a quick port or a net performance win.

[integration]: https://github.com/MaSieS4Fun/ARM_bloodborne_pc/commit/3d56243dfc8a919aceb96b94bf69b1e6ec1171cc
[embedding]: https://github.com/MaSieS4Fun/ARM_bloodborne_pc/blob/3d56243dfc8a919aceb96b94bf69b1e6ec1171cc/cpu/guest_cpu_fex.cpp
[bridge]: https://github.com/MaSieS4Fun/ARM_bloodborne_pc/blob/3d56243dfc8a919aceb96b94bf69b1e6ec1171cc/src/guest_cpu.h
[fixture]: https://github.com/MaSieS4Fun/ARM_bloodborne_pc/blob/3d56243dfc8a919aceb96b94bf69b1e6ec1171cc/tests/test_guest_cpu.c
[fex-cmake]: https://github.com/FEX-Emu/FEX/blob/9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf/CMakeLists.txt
[code-buffers]: https://github.com/FEX-Emu/FEX/blob/9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf/FEXCore/Source/Interface/Core/SharedCodeBufferManager.cpp
[apple-jit]: https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon
[apple-abi]: https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
[fex-tso]: https://fex-emu.com/Scourge-of-emulation/
[fex-defaults]: https://fex-emu.com/FEX-2406/
