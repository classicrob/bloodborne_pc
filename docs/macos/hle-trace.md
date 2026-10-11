# Filtered HLE import timeline

`BB_HLE_TRACE=sync,gnm,videoout,avplayer` wraps selected resolved host function
imports at `runtime_resolve`. Data imports and linked native guest exports are
not wrapped. Unset/empty/`0` returns the original pointer; there is no per-call
branch or trampoline when disabled. This is an opt-in diagnostic, not a timing-
neutral observation.

Filters may be comma-separated categories or exact symbol/scoped-NID names:

- `sync`: mutex, condvar, semaphore, event flag, equeue, usleep/nanosleep, yield,
  and clock-gettime calls.
- `gnm`: Gnm names containing Submit, Wait, Flip or Label. Builders are excluded
  to retain the handoff timeline. Select a builder by its exact symbol if needed.
- `videoout`: all sceVideoOut imports.
- `avplayer`: all sceAvPlayer imports.

Each ENTER/EXIT record contains a global sequence, paired call ID, raw TSC,
pthread identity/name, import symbol, original caller, and first four integer
argument registers. EXIT adds RAX/RDX return bits, elapsed TSC ticks, and the
number of intervening traced frames abandoned by nonlocal control transfer.
Integer fields are hexadecimal. Duration includes tracing overhead and is not a
CPU-time measurement. Floating argument/result bits are preserved by the bridge
but are not decoded into these four integer-argument fields.

The ring retains 8192 records (roughly 4096 complete calls, less if calls remain
pending); overwritten records are discarded oldest first. `runtime_report` dumps
it. The exported `runtime_hle_trace_dump` is also the parser-stop integration
point. `BB_HLE_TRACE_STREAM=1` additionally writes records as calls run, at greater
cost. Concurrent streamed lines may appear out of sequence; use the sequence
field. Normal imports use a short global atomic gate to publish fixed records.
A fatal dump attempts that gate once: if interrupted during publication or
another dump, it prints an unavailable marker rather than spinning or reading
an incomplete record. Dump formatting uses fixed stack buffers and write(2),
without TLS lookup, allocation, name lookup or pthread locks.

The x86-64 bridge preserves stack arguments, AL, integer registers, flags, x87,
XMM and YMM state around logging. Host errno and guest TLS errno are preserved.
It changes the function's return PC, so older diagnostics using
`__builtin_return_address` inside a traced HLE function see the bridge; the ring
contains the original caller. Target stack arguments are not copied. Callers
cannot retain red-zone values across a function call under the SysV ABI.

Per-thread bookkeeping is bounded to 1024 calls and keys them by their original
return slot. On return it discards skipped inner frames; repeated calls at a
reused slot replace stale records. This handles a callback longjmp back into an
outer traced function, followed by that outer function returning. Calls abandoned
without a matching outer return can retain entries until slot reuse; exhausting
the bound stops loudly. There is no C++ exception unwind metadata for these
bridges: exceptions unwinding across a traced import are not qualified. Do not
use this diagnostic to claim normal exception/unwind behavior.

`tests/test_hle_trace_macos.sh` is wired into Darwin `--test`. The fixture covers
disabled pointer identity and filtering, ten integer arguments (including stack
arguments), variadic FP, x87 long-double and aggregate returns, YMM arguments and
return, nested callbacks, repeated inner longjmp with outer return, both errno
domains, concurrent ring wrap, and a signal/fault interrupting publication on the
publishing thread. Logs from a busy fatal dump explicitly state their limitation.
