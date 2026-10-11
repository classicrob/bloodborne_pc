# Guest function-entry trace on Rosetta

An opt-in loader diagnostic logs selected guest function entries without a
debugger or a game-source patch. For the movie context-copy investigation:

```sh
BB_GUEST_ENTRY_TRACE=0x26aeab0,0x21676c0,0x2168080,0x26aee20,0x26af230,0x26aec40 bash run.sh
```

The offsets are relative to the linked image and were statically checked in
EU v1.09. They select context copy-assignment, child-context factory,
initialization from a parent, context initialization, out-of-space callback,
and context destruction. No offset is hard-coded into the implementation.
Installation prints each offset and absolute site/trampoline address.

Each `GUEST-ENTRY` hit prints a process-local atomic sequence, Mach absolute-time
ticks, the guest thread handle (from the established GS TCB slot), saved caller
at RSP, all GPRs/flags and eight qwords at each of RDI, RSI and RDX. For the copy,
RDI is destination and RSI source; qwords 0/1/2/3/4 are begin/end/cmdptr/callback/
userdata. For initialization from a parent, RDX names the parent. An unreadable
pointer is recorded with a Mach error instead of being dereferenced.

Sequence numbers order handler entry, not object mutations by other threads.
The three object reads are not a coherent cross-thread snapshot. Treat them as
observations, not ownership or race-free evidence. Sort concurrent output by
sequence if the writes reach stderr in a different order. Thread names and
builder call chains are provided separately by the PM4 builder diagnostic.

## Supported patch and safety constraints

The environment setting is a list of at most 16 executable image offsets.
Every selected entry must begin with byte `55` (`push rbp`). Unsupported entry
instructions, duplicate sites and invalid offsets fail startup loudly. This is
intentionally a narrow instruction-relocation contract, not a general x86
relocator. In particular, leaf CommandBuffer initializer `0x1071270` is not a
supported entry; probe its parent initializer instead.

Installation happens before linked module initialization or guest entry, while
guest execution is quiescent. Metadata and anonymous RX trampolines are ready
before the entry byte becomes INT3. Removing execute permission while patching,
then restoring it, invalidates an earlier Rosetta translation. Each breakpoint
remains installed permanently. The saved RIP convention measured by the test
is site+1; only SIGTRAP/TRAP_BRKPT with x86 trap number 3 at configured sites is handled. Other traps retain the
loader's fatal path.

The signal handler redirects RIP to a trampoline containing the displaced
`push rbp` followed by RIP-relative indirect jump with an inline absolute target
back to site+1. It neither unpatches the entry nor single-steps, so concurrent
entries cannot bypass a temporary patch gap. It does not use register scratch
or alter flags to resume. The handler uses per-invocation bounded stack buffers,
a lock-free sequence counter, cached GS displacement, safe Mach reads and
write(2). It preserves errno. No allocation, printf, pthread lock, name lookup,
or debugger API runs in the handler.

This is an observation tool, not a scheduling-neutral trace: every selected
entry incurs a signal, bounded kernel reads and log output. Keep the site list
small. Do not use enabled runs for performance claims or silently enable the
probe in normal play.

## Test

```sh
tools/macos/heavy.sh guest-entry -- sh tests/test_guest_entry_macos.sh
```

The signed SDK-only x86_64 fixture first executes the unpatched function to
prime Rosetta, then installs the actual probe implementation. It checks all
GPRs, arithmetic flags, all 16 YMM registers, saved RBP, pre/post RSP and caller
return address across 34 calls, including four concurrent workers. It checks
invalid/non-executable/duplicate sites, safe invalid data reads and absent TCB,
rejects user/trace provenance even at a matching PC, and verifies an unrelated
SIGTRAP reaches the fatal handler. No guest game
bytes are included in the fixture.
