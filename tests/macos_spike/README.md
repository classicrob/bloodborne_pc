# Rosetta feasibility spike

Run `tests/macos_spike/run.sh` on Apple Silicon with Rosetta installed. Only the
macOS SDK and Clang are required. Outputs go in `out/macos_spike/`. Every probe
runs in a separate child with a 20-second timeout and core dumps disabled. Exit
1 means at least one capability probe failed; unsupported capabilities are
expected, and do not necessarily mean the port is infeasible. This is a
non-hardened command-line executable; signing/sandbox entitlement policies for a
future application bundle need separate validation.

`tests/macos_spike/run.sh --vmmap` prints its PID and waits, allowing `vmmap PID`
from another terminal. Terminate that waiting process after inspecting it.

## Observed on 2026-10-07

Apple M4 Max, Darwin 27.0.0, Apple Clang 21.0.0. `sysctl.proc_translated=1`,
`uname.machine=x86_64`, `sysconf(_SC_PAGESIZE)=4096`. Captured output is in
`results.txt` (repeated successful/blocked survey intervals are condensed).

| Capability | Observed result | Port consequence |
|---|---|---|
| 9-byte `65 48 8B 04 25 disp32` GS load | Keys 258–261; displacement `8*key`; main and three concurrent new pthreads read independent values | Use a once-created pthread key for the guest TCB |
| Entire 64 GiB–1 TiB fixed reservation | `EACCES` | Cannot reserve the original guest arena |
| Address survey with non-overwriting Mach reservations before MAP_FIXED | 5–8 GiB, 9–63 GiB, 448–1024 GiB accept 1-GiB reservations; 4–5 and 8–9 contain loaded mappings; 63–448 unavailable | Keep host arena at 32 GiB, end before 63 GiB; move guest arena minimum to 448 GiB |
| `vmmap` guard | `fc0000000-7000000000 [385.0G] ---/--- SM=NUL` | The 63–448 GiB guard explains the observed rejection; not evidence of which Rosetta/kernel component creates it |
| Contiguous 448 GiB–1 TiB | 576 GiB PROT_NONE reservation succeeds | More than the required guest address space remains |
| `shm_open`, unlink, truncate, MAP_SHARED fixed views | Four coherent aliases, including 1 TiB minus 64 KiB; 4-KiB object offsets work | Shared data views are feasible |
| `mprotect` 4 KiB and 16 KiB | Both work; neighboring pages remain writable | Rosetta exposes useful 4-KiB protection granularity |
| RO-page write | SIGBUS (10), code 2; `__ss.__rip` exactly matches fault instruction; `__es.__err=6` | Handle SIGBUS and write bit 1; mprotect in handler and retry succeeds |
| `thread_get_state(x86_THREAD_STATE64)` | Succeeds for current and another waiting pthread | Darwin watchdog can retrieve translated x86 state |
| Anonymous low executable memory | RWX and RW→RX work; 100 patches after first execution produce fresh results | Same-view code patching works without explicit cache flush in this test |
| shm-backed executable memory | RWX initial execution SIGBUS; RW→RX mprotect EACCES | Do not use shm objects as executable backing; Astra-2 owns Mach alias alternative |
| malloc 16 B–16 MiB | All observed pointers above 1 TiB | Explicit low allocator required for guest-visible objects |
| Low mmap | Hint and fixed low mappings work | Below-1-TiB allocations are possible |
| SSE4.1, SSE4.2, AES, POPCNT | CPUID advertises; representative instructions execute and results match | Basic ISA probes pass |
| AVX (256-bit), F16C, BMI1, LZCNT | CPUID hides; representative instructions execute and results match | CPUID alone understates these observed capabilities; not an exhaustive ISA test |
| AVX signal state | All 16 YMM registers preserve full 256 bits across SIGBUS despite `vzeroall` in handler | Tested fault/retry preserves vector state |
| BMI1 BEXTR | Extracted value and zero flag match | Instruction present in real eboot allocation code works |
| SSE4a EXTRQ and INSERTQ | CPUID absent, both tested immediate forms SIGILL | Guest use requires a patch/emulation path |

The shared-code alias probe fails while adding execute permission, so it says
nothing about cache invalidation through a *working* executable alias. See
Astra-2's separate Mach alias experiment for that question. No `MAP_JIT` or
special code-signing entitlement is used here.

## Address and instruction audit

`src/probe.c` allocates the whole image with `allocate()` / `runtime_low_map()`;
segment addresses and module bases are offsets from that allocation. Conversion
in `scripts/prepare.py`, `scripts/link_libc.py`, and `scripts/link_modules.py`
caps the aggregate image at 512 MiB and records base-relative relocations.
Moving the host allocation does not require converter relocation changes.
Guest main and pthread stacks also use `runtime_low_map()`. `USER_MIN` in
`runtime_memory.c` is the first-fit guest VMA minimum, while `[LOW_MIN, USER_MIN)`
was the separate host allocation arena. Apple must give these independent bounds.
Guest fixed requests in the guard cannot work; there is no game dump available
yet to inspect actual requests or scan instruction usage.

No guest SSE4a translator or CPUID dispatch was found in local `src/`, `scripts/`,
`gpu/shim/`, or the vendored `gpu/shadps4/common/`. The local shadPS4
`memory_patcher.cpp` concerns game patches, not CPU instruction translation.
The vendored SRT walker uses Xbyak code generation but has no `Cpu::has` feature
branch. Its inclusion of `xbyak_util.h` alone is not feature dispatch. The Xbyak
submodule and FFmpeg sources were not populated in this worktree during audit.

Upstream shadPS4 has the relevant [CPU patcher](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/cpu_patches.cpp):
`FilterNoSSE4a` checks Xbyak `Cpu::tSSE4a` and EXTRQ/INSERTQ have translation
handlers. This file is not vendored into bbport. Any adoption must preserve its
license and adapt its decoder, trampolines, and signal integration. Whether the
game needs it must be answered by a real executable scan or a SIGILL trace.

[FFmpeg's x86 CPU detection](https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/x86/cpu.c)
gates AVX on CPUID and OS extended-state support. With this spike's hidden bits,
normal detection should choose older implementations. Do not force global AVX
flags from these few instruction probes; execution has not been tested across the full instruction family. The targeted
SIGBUS test preserves all sixteen full YMM registers.

## Runtime integration regression

`tests/macos_spike/test_tls_runtime.sh` tests the actual `runtime_thread.c` and
`probe.c` TLS rewrite under Rosetta. It runs once with an isolated low allocator
fixture (including injected stack-allocation failure), then with the actual
`runtime_memory.c` Mach backend and reusable guest object allocator. Both verify
concurrent key initialization, the nine-byte rewrite, executable segment bounds,
main/host/guest pthread isolation, copied and zeroed static TLS, 8192-byte TLS
alignment, and low addresses for TCB, DTV, errno, handles, attributes and guest
stack. This is SDK-only and does not link or build the GPU library.

`tests/macos_spike/test_hle_stack.sh` exercises the actual C++ HLE thread adapter
with the real C runtime and Mach allocator. It checks low callback stack/TLS
addresses, native-stack restoration, pooled stack reuse, concurrent workers,
stop/join and self-stop semantics, concurrent/repeated join completion, destruction
ordering, and a child process whose callback exception
must fail loudly. The adapter catches exceptions on the low stack before they
could unwind across the assembly stack bridge. No GPU library build is needed.

## macOS 27.2 revalidation

After the host updated from macOS 27.0 to 27.2 (build 26B5101f), rerunning
`sh tests/macos_spike/run.sh` on 2026-10-07 reproduced all 25 probe outcomes.
[The new capture](results-macos27.2.txt) collapses consecutive identical 1-GiB
reservation results using the same format as [the 27.0 capture](results.txt).
The differences are the OS version, concurrent-thread print order and ASLR
addresses. Keys 258–261, 4096-byte pages, the protected 63–448 GiB interval,
SIGBUS/code 2/write-error 6, and instruction behavior are unchanged. Exit 1
still represents the same five unsupported probes: three executable shm-view
cases and the two SSE4a instructions. This SDK-only spike does not test MoltenVK.
