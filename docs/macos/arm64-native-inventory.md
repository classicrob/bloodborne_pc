# Native arm64 host inventory

Status: source audit at `b6ec469`, 2026-10-07, macOS 27.2 (26B5101f).
This is groundwork for a later x86-64-to-arm64 CPU layer, not its implementation
plan. No ARM GPU build, executable-code experiments, or production changes were
made for this inventory. Live Rosetta failures take priority over this work.

A native host build still needs to execute the game's **x86-64 eboot and native
PRXs**. Compiling HLE/runtime code for ARM does not replace that execution layer.
The current loader places real host function addresses in guest relocations and
calls guest addresses as C function pointers. Both directions must change.

## Scope, evidence, and estimates

Audited `src/`, `gpu/shim/`, the common/video-core/shader-recompiler and HLE sources
selected by `gpu/CMakeLists.txt`, launcher/build scripts, and guest linker scripts.
Unselected upstream shadPS4 CPU emulation code is not part of this host build.
Third-party libraries are listed as dependency work rather than claimed to have
been exhaustively audited. No proprietary bytes are included here.

Strict `clang -arch arm64 -std=c11 -Wall -Wextra -Werror -fsyntax-only` over all 21
`src/*.c` translation units: **19 pass, 2 fail**. The failing units are `probe.c`
(x86 guard and register contexts) and `runtime_kernel.c` (x86 intrinsic headers).
Includes used the existing shared dependency headers and LibAtrac9 source headers;
no ARM libraries were linked. `compat_win.c` is empty on Apple. Logs are local
`out/arm64-inventory/summary.txt` and per-file diagnostics.

A pass does not establish portability: file-scope assembly in `runtime_thread.c`
is not assembled by `-fsyntax-only`, and `sysv_abi` declarations do not create an
x86/ARM translator. The actual restart regression was independently rerun under
Rosetta on 27.2 and passed both default and custom path cases. Astra-1 reports all
22 Rosetta spike outcomes unchanged from 27.0, including the existing five negative
probes. These results describe Rosetta, not native ARM page or address behavior.

Sizes below are rough engineering estimates including focused regression tests,
assuming an agreed CPU-layer API already exists: **S = 0.5–2 engineer-days,
M = 3–7 days, L = 2–4 weeks, XL = larger/unknown**. They overlap and must not be
summed into a delivery promise. CPU translation correctness and performance are
outside these host-port estimates.

## Host assumptions and work items

| Item and source anchors | Current assumption / required native behavior | Size |
| --- | --- | --- |
| Build and launcher: `build.sh:24`, `gpu/CMakeLists.txt:5`, `packaging/macos/build-deps.sh`, `run.sh` preflight, `probe.c` `--verify-macos-host` | Forces `-arch x86_64`, x86 dependency prefix, non-PIE, and translated-process verification. Add a deliberate native target with matching libraries and CPU-layer verification. ARM64 non-PIE support must not be assumed. Keep the working Rosetta configuration explicit. | M |
| Loader gates and entry: `src/probe.c:10`, `enter_on_stack`, linked-module initializers, final guest entry | Compile-time x86-only guard; handwritten `%rsp/%rbp/%rdi/%rsi/%rcx` trampoline; raw calls into ELF bytes. Replace execution entry with CPU-layer entry, including module initialization and finalizers. Native stack switching is a separate ABI-specific operation. | L, CPU engine excluded |
| Import binding and unresolved imports: `src/probe.c` relocation loop and generated `t[0..19]` stubs | Writes native function addresses into guest slots; emits `mov rsi,rdi; mov edi,index; movabs rax,handler; jmp rax`. ARM addresses cannot be executed as x86. Use guest-addressable call gates with typed signatures; preserve missing-import argument and guest return-RIP diagnostics instead of `__builtin_return_address` on a native frame. | L |
| C/C++ HLE ABI: `src/runtime.h:22`, `gpu/shadps4/common/types.h:28`, all `RuntimeExport` / HLE registration tables | `ABI` and `PS4_SYSV_ABI` are `sysv_abi`. Export tables carry function pointers without complete machine-readable signatures. ARM function pointers need typed call adapters with guest data layouts, integer/FP/aggregate results and stack arguments preserved. Simply deleting the attribute is insufficient. | L–XL; signature census required |
| Calls back into guest: `runtime_thread.c` entry, once/key-destructor/cleanup paths; `runtime.c` atexit/cxa callbacks; AvPlayer callback structs; `gpu/shim/core/libraries/kernel/threads.h` | Pthread/HLE workers currently invoke guest function pointers directly, sometimes nested inside a guest-to-host call. Route every such invocation through reentrant CPU contexts. Include callback payload ownership and below-1-TiB pointers from `docs/macos-guest-pointer-audit.md`. | L |
| HLE low-stack bridge: `runtime_thread.c` `bb_macos_call_on_stack` | Apple-only assembly is still x86. Native ARM switch must preserve callee-saved registers, FP/LR, alignment and platform-reserved registers. Decide which host callbacks still require low native locals when guest stacks become explicit CPU-layer state; never hand the guest a high native stack pointer. | M |
| TLS: `runtime_thread.c` `runtime_thread_tls_displacement`, key installation, TCB/DTV; `probe.c` `rewrite_platform_tls`; `scripts/link_libc.py`, `scripts/link_modules.py` `FS_LOAD` | Linkers rewrite a specific nine-byte `FS:[0]` load to GS; Rosetta uses `GS:[8*pthread_key]`. Native ARM has no guest GS register. Preserve guest FS/GS bases in CPU state and the existing guest TCB/DTV/errno layout. Keep host pthread TLS separate. Choose original or rewritten guest input deliberately; do not accidentally interpret the Rosetta key displacement as guest TLS. | M–L |
| Signals and diagnostics: `probe.c` `CONTEXT_*`, `fault`, `dump_frames`, `watchdog`; `gpu/shim/bbport_write_log.cpp` | Apple code directly reads `__rip`, `__rbp`, `__rdi`, eight named GPRs, and requests `x86_THREAD_STATE64`. Native signals provide ARM host state. Add guest-state recovery/guest RIP mapping and separate host-frame diagnostics; watchdog needs published guest state, not an ARM PC mislabeled as RIP. | L |
| GPU fault utilities: `gpu/shadps4/common/signal_context.cpp`, `gpu/shim/core/signals.h` | Common utility already has Apple ARM `__pc` / ESR write-bit branches. That gives **host** PC/access direction only. Preserve GPU write-tracking recovery, distinguish CPU translated-code faults from native helper faults, and resume the correct instruction. Apple dispatch currently filters to writes. | M, overlaps signals |
| Host SRT JIT: `shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp` (about 890 lines) | Unconditional Xbyak includes, 32-MiB x86 code buffer, x86 register allocation, walker function pointers and code copying; non-x86 functions abort. Host shader-resource-table walking needs a complete ARM implementation or exact-semantics portable evaluator. This is CPU host work even though it lives in the shader recompiler. | L |
| SRT self-patching: same file, `SrtWalkerSignalHandler` | Uses Zydis to decode a faulting x86 MOV and overwrites it with XOR/NOP bytes. Native implementation must reproduce the intended resource-read result without assuming x86 encoding or executable-memory permissions; also reconcile read-fault recovery with the Apple write-only dispatcher. | M–L, overlaps SRT |
| Guest clock: `src/runtime_kernel.c:88`, `read_tsc`, `tsc_frequency` | Unconditional `<x86intrin.h>` and three `__rdtsc()` sites. CPU-layer RDTSC/RDTSCP and `sceKernelReadTsc`/frequency must share one consistent guest clock domain. Direct ARM counter substitution in only HLE would split the clock. | M |
| Host profiling clocks: `bbport_write_log.cpp`, `video_core/renderer_vulkan/vk_draw_pipe.h`, `vk_bind_helper.h`, `vk_rasterizer.cpp` | Unguarded x86 intrinsics in logging/worker timing. Use an architecture-neutral host clock and matching frequency/units; keep guest-visible timestamps separate. Common `rdtsc.h` already has an ARM `CNTVCT_EL0` implementation and `NativeClock` consumes it; audit all direct callers. | S–M |
| Virtual memory: `runtime_memory.c`, `probe.c` mappings; `video_core/page_manager.cpp` | Rosetta arenas assume low `[32,63)` GiB and guest `[448,1008)` GiB. Native address availability requires a fresh probe, not inheritance of Rosetta's guard. Guest runtime pages are 16 KiB, while GPU protection logic uses 4-KiB host granules. A native host with larger pages needs protection aggregation with correct subpage tracking. | L |
| Executable aliases: `runtime_memory.c` backing writes and execute-permission cycling | Current remap aliases plus RX off/on invalidate Rosetta translations. Native translated-code cache must invalidate all blocks derived from changed guest bytes, including aliases. ARM instruction-cache maintenance and code publication are additional requirements, not replacements for guest-block invalidation. | L, overlaps CPU engine |
| Other page assumptions: `runtime_file.c` `touch_for_write`, GPU page/range trackers, tests | Hard-coded 4096-byte touching/protection and bit shifts need separation into guest granularity and host protection granularity. File-stat `blksize=4096` is guest metadata, not necessarily a host-page bug. Test nonaligned ranges and adjacent independently tracked guest subpages. | M |
| Nonlocal control transfer: `runtime_thread.c` thread-exit `setjmp/longjmp`, `runtime_memory.c` `runtime_fault_recover` | Host jumps currently bypass directly executed guest frames. CPU layer must abandon/restore its own execution frames and guest state consistently, including nested callbacks; guest libc setjmp/longjmp remains x86 guest behavior. | M–L |
| Shared structures and atomics: runtime sync/memory hooks, guest GPU labels and command data | x86 and ARM64 are both 64-bit little-endian, but that does not prove structure/bitfield/atomic compatibility. Retain explicit guest structs and `_Static_assert`s, audit unaligned casts and atomics shared with translated code, and enforce publication order on guest/HLE/GPU-worker boundaries. | L audit, defects unknown |

## Existing portability support and exclusions

- `common/arch.h` recognizes ARM64. `common/spin_lock.cpp` uses ARM `yield` and
  acquire/release atomics. `common/assert.cpp` has an ARM `brk` path (it is not in
  the current `COMMON` source list). These do not require an x86 emulator.
- `common/bit_array.h` guards AVX2 operations with `__AVX2__`; scalar paths exist.
  Use and test the scalar implementation first; NEON optimization is optional.
- `common/va_ctx.h` explicitly models the x86 register-save area and substitutes
  `__int128` for `__m128` off x86. That substitution is **not** an ABI bridge.
  No use of this header was found in the currently selected GPU/HLE sources; it
  remains a risk if additional upstream libc/HLE sources are enabled.
- `bbport_overlay.cpp` file-level assembly embeds font data in Mach-O sections;
  it contains no x86 instructions. Verify section/linker details, but do not
  classify every `asm` token as a CPU translator requirement.
- `compat_win.c` x86 assembly and Windows TEB/GS paths are Windows-only and must
  retain their current behavior. Linux `arch_prctl` is already excluded on Apple.
- Zydis decodes **guest x86** and SRT-generated x86. Its presence alone is not a
  host-ISA blocker; retain a guest decoder if needed, remove SRT dependence only
  when that implementation changes. Guest patch byte patterns in linker/patch
  scripts remain intentionally x86 data, not instructions executed by Python.
- LibAtrac9 plus actual AJM already produced byte-identical real-asset PCM in ARM
  and Rosetta builds. SDL, FFmpeg, Vulkan/MoltenVK and remaining libraries need
  native slices/configuration and tests; they must not link the x86-only prefix.
  Bundled FSR, ImGui, xxhash, half, Zydis and template/header dependencies were not
  rebuilt or exhaustively preprocessed here. Reserve M–L for dependency/build
  integration and further syntax findings, not a claim they all compile today.

## Required CPU-layer contract

1. **Guest architectural state and address space.** Execute eboot and native PRXs,
   including indirect calls and dynamically changed guest instructions. Preserve
   64-bit guest addresses, the below-1-TiB pointer convention, GPRs/RFLAGS,
   x87/SSE/AVX state and required instruction semantics. An executable guest address
   and a native host entry point must be distinguishable even if represented by
   the same C integer type. Define CPUID, traps and privileged/unsupported behavior
   explicitly; current Rosetta execution evidence is not a future ARM CPU contract.
2. **System V guest calls in both directions.** Marshal the guest's six integer
   argument registers, vector argument registers, stack arguments, alignment,
   red-zone use, aggregate/hidden-return rules and returns into Apple ARM64's ABI.
   Preserve guest callee-saved state and call/return semantics through callbacks.
   Variadic calls require explicit guest register-save/overflow-area handling,
   not native `va_list` reuse. Retain guest return addresses for import diagnostics.
   Apple reserves x18 and uses a different variadic convention; consult its
   [ARM64 ABI documentation](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).
3. **Per-thread guest TLS and lifecycle.** Each guest thread and each host worker
   entering guest code needs a persistent guest execution context, FS/GS bases,
   low TCB/DTV/errno and guest stack. Attach/detach, key destructors, once callbacks,
   reentrant host calls and guest pthread exit must act on that same context.
4. **Fault delivery and restart.** For each faulting translated host PC, recover
   guest RIP, fault address, access kind and sufficient guest registers to retry,
   emulate or stop. A GPU write-protection fault must resume with exactly one
   completed guest store. Distinguish native helper bugs, unmapped guest accesses,
   illegal guest instructions and intentional speculative-read recovery. Publish
   safepoint state for watchdogs; never report a generated ARM PC as a guest RIP.
5. **Memory ordering and atomicity.** Preserve x86 ordering for ordinary guest
   memory as well as LOCK/RMW instructions and fences. Correct host C++ atomics
   alone cannot constrain plain translated guest loads/stores. Cover unaligned
   and cross-boundary atomics, store-buffer behavior, self-modifying code, and
   interactions with HLE writes and GPU command/label synchronization. Test with
   litmus cases and mixed guest/host producer-consumer tests before optimizing.
6. **Code lifetime and clocks.** Expose mappings/protection/remap/invalidation to
   the translator; map all physical aliases to affected translations. Publish new
   ARM code with proper W^X and cache maintenance, and retire code safely while
   other threads execute it. Supply one guest TSC/frequency model shared with HLE.
   AOT does not eliminate these requirements if guest pages can change at runtime.

## Apple TSO: what a normal native app can rely on

**Planning answer: do not assume a supported userspace macOS TSO switch exists.**
Apple documents native Apple-silicon memory ordering as weaker than Intel's and
requires proper synchronization. No public native-macOS TSO opt-in was found in
this audit. This is a supported-API conclusion, not proof that no private or future
mechanism exists. See Apple's
[architecture differences](https://developer.apple.com/documentation/apple-silicon/addressing-architectural-differences-in-your-macos-code).

Apple does document `ACTLR.TSOEN` and a per-thread
`prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)` interface, but specifically for a
**patched Linux kernel inside Virtualization.framework**, not an ordinary macOS
process. That is not an API bbport can call on Darwin. Source:
[Accelerating Rosetta](https://developer.apple.com/documentation/virtualization/accelerating-the-performance-of-rosetta).

The author-maintained [TSOEnabler implementation](https://github.com/saagarjha/TSOEnabler)
provides another concrete distinction: it installs a kernel extension to expose
`kern.tso_enable` per thread; that sysctl is supplied by the extension, not a
standard application facility. Its old M1-specific implementation and security/
installation requirements are not a normal-app deployment solution, nor evidence
of support on this M4/macOS 27.2 host. Nothing was installed or toggled.

Therefore native CPU-layer estimates must include software enforcement of x86
memory ordering unless a separately validated, supported API changes this premise.
Do not silently enable a weaker ARM execution mode. Hardware TSO would also not
solve ABI marshaling, guest instruction semantics, precise faults or code-cache
coherency.

For generated native code, follow Apple's
[JIT porting guidance](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon):
account for executable-memory entitlements/write protection and invalidate the
instruction cache after writing instructions. Signed AOT text can avoid some JIT
allocation work, but runtime call gates, SRT generation and patched guest-code
translations still need a defined policy. This inventory selects no recompiler
or code-generation implementation.
