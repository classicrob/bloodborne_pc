# CUSA03173 v1.09 eboot static audit

2026-10-07, SHA-256
`d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9`.
Plaintext SELF, one executable PT_LOAD at image offset 0 with 84,776,668 file
bytes. No defined function symbols in the dynamic symbol table. No game bytes
are included in this repository.

**No SSE4a EXTRQ/INSERTQ candidates were found**, including a scan at every
possible instruction start with mandatory 66/F2 prefix, optional REX, and
0F 78/79 opcode, confirmed with Zydis. This covers both immediate and register
forms. There are therefore no SSE4a functions/hot paths to enumerate in this
eboot, and no evidence requiring a patcher for it. Base system modules remain
unscanned until extracted. Runtime-generated code remains outside this audit.

`prepare.py` parses the SELF successfully but cannot produce boot.bin without
`sce_module/libc.prx`. The shared `parse_self` and `link_modules.module` helpers
can inspect the eboot independently; the executable segment was extracted into
`out/macos_spike/eboot/code0.bin` for scanning. Total scratch is about 85 MB.

## Mapping calls

NID symbol relocations identify four memory imports and their PLT stubs. These
are image-relative offsets, not process virtual addresses.

| API | PLT stub | Observed call sites | Address / flags |
|---|---|---|---|
| sceKernelAllocateDirectMemory | 0x2bbeab8 | 0x33ead5, 0x2081a9e, 0x2081ebc, 0x26d3ee0 | Physical allocator, not a fixed virtual request |
| sceKernelMapDirectMemory | 0x2bbeac8 | 0x33eb0d, 0x2081ac1, 0x2081ee9 | Zero address, flags 0 |
| sceKernelMapDirectMemory | 0x2bbeac8 | 0x26d3fda | Helper-generated address hint, flags 0 |
| sceKernelMapDirectMemory | 0x2bbeac8 | 0x29032b9 | Zero address, flags 0x80 (NO_OVERWRITE) |
| sceKernelReserveVirtualRange | 0x2bc01f8 | 0x2901fbd | Zero address, flags 0x80 |
| sceKernelMapFlexibleMemory | 0x2bc0d48 | 0x2081dbe | Zero address, flags 0 |

No MAP_FIXED request into the blocked 63–448 GiB guard was found at these sites.
Nonfixed hints can be moved to the valid arena. Many 64-bit immediates have a
numeric value in the guard range, but nearby instructions use packed fields,
bitmasks, and other non-address constants; raw constant matching does not prove
an address dependency. This is a static call-site audit, not proof excluding
indirect calls, dynamically computed requests, or calls from base modules.

## Other ISA candidates

The executable segment includes embedded data and shaders, so blindly decoding
all 84 MB as x86 produces false AVX512/3DNow/etc. counts. In the code region
before PLT (0..0x2bbe000), the targeted uncommon-ISA filter (AVX2, AVX512, FMA,
3DNow, XOP, CLZERO, MONITOR, RDSEED, RTM) found only two XBEGIN decodes at
0x177e72 and 0x18bb42. Both occur in data between function bodies, surrounded
by invalid/privileged decodes, before aligned function prologues. They are not
credible executable paths. SSE4a candidate-start scanning covered the entire
segment, independently of this code-region heuristic.

AVX and BMI1 instructions are visibly present in real allocation code (including
YMM loads/stores and BEXTR), reinforcing the need for the execution probes rather
than rejecting the process from CPUID alone. The spike's selected instruction
checks are not an exhaustive validation of the Jaguar ISA or vector-state
preservation over signals.

## Reproduce the decoder pass

The original `spike.c` remains SDK-only. This optional `scan_x86.c` audit utility
requires Zydis 4.1.1 and Zycore in `BB_DEPS_PREFIX`:

```sh
xcrun clang -arch x86_64 -O2 -Wall -Wextra -Werror \
  -I"$BB_DEPS_PREFIX/include" tests/macos_spike/scan_x86.c \
  "$BB_DEPS_PREFIX/lib/libZydis.a" "$BB_DEPS_PREFIX/lib/libZycore.a" \
  -o out/macos_spike/scan_x86
arch -x86_64 out/macos_spike/scan_x86 out/macos_spike/eboot/code0.bin
```

Pass start/end offsets to print a local disassembly. PLT target filters and the
code-region boundary in this utility are specific to the audited eboot hash.
Do not reuse those filters for another game or module without updating them.

## Bundled PRX follow-up

The read-only, verified base+v1.09 installation became available later the same
day. All seven `sce_module/*.prx` files were parsed and their executable segments
scanned with Zydis and the same all-start SSE4a candidate check. **All seven had
zero EXTRQ/INSERTQ candidates.** File hashes and segment sizes are recorded in
`module-audit.json`; no proprietary module bytes are committed.

Only libc imports a mapping API from the searched set. Its PLT stubs are
`sceKernelMapNamedFlexibleMemory` at 0x350 and `mmap` at 0x4b0. Raw relative-call
candidates were checked with native LLVM disassembly (no translated execution
needed):

| libc call offsets | API | Address and flags |
|---|---|---|
| 0x23ec0, 0x23ff9, 0x24932 | sceKernelMapNamedFlexibleMemory | Address pointer initialized to zero; flags 0 |
| 0x52fac, 0x54f25, 0x5536d, 0x578c5, 0x57912 | mmap | Address zero; flags 0x1002 (anonymous/private), no MAP_FIXED |
| 0x530e1 | mmap | Address zero; flags 2 (private file mapping), no MAP_FIXED |

None of these direct call sites requests a fixed address in the guard. Indirect
calls and dynamically generated code remain outside this static guarantee.

## First real game runs

With the verified libc and Fios2 linked, the unmodified CPU-only debug run:

- Patched 17,127 guest TCB loads and applied 237,298 relocations.
- Ran both native module initializers successfully.
- Created 16 guest threads, including FMOD audio/mixer/stream and GXRenderThread.
- Allocated 4,812,046,336 bytes of direct memory in the valid low guest arena.
- Opened two CoreAudio ports and emitted five audio buffers.
- Stopped explicitly at `sceGnmGetTheTessellationFactorRingBufferBaseAddress`
  (guest return offset 0x26d3fb9), because `--cpu-only` does not register Gnm.

No ISA/TLS/protection fault occurred in that run. This is initialization evidence,
not evidence of the game main loop or successful rendering. The team chose not
to add fake CPU-only graphics behavior: milestone testing proceeds on the real
GPU path. That path then stopped with exit 23 at the required `nullDescriptor`
feature check on MoltenVK, before guest module initialization.

A separate local, unmerged capability-bypass artifact was prepared only for
later blocker discovery. It was not run at the time of this report because
newly compiled x86_64 processes began stalling before main system-wide. None of
the results above used that artifact; it has no role in correctness acceptance.
