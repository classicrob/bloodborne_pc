# Chained indirect buffers are jumps (cross-platform parser bug)

## Bug

`IT_INDIRECT_BUFFER` and `IT_INDIRECT_BUFFER_CONST` carry a CHAIN bit (bit 20 of the size dword;
`PM4CmdIndirectBuffer::chain`). With it set, the command processor jumps to the new buffer and never
returns, so the rest of the buffer holding the packet is not executed. Gnm uses this to link
command-buffer chunks. After the chain packet a chunk holds whatever its memory last held: zeros or
stale packets.

shadPS4/bbport's `Liverpool` parsers treated every indirect buffer as a call. After the chained
buffer finished, they returned and parsed the dead tail. Results:

- **A zeroed tail:** a zero header, `Unimplemented PM4 type 0, base reg: 0, size: 1`, which is a
  fatal stop.
- **A stale tail:** misaligned garbage (`Wrong PM4 type 1`, `NextPacket: packet length exceeds
  remaining submission size`), or old packets executed a second time.

## Evidence (Bloodborne v1.09, macOS, 2026-10-10)

Default-config runs tore at scene loads (the movie start, entering character creation, after
"Finalise contract"):

- With the GpuIdle fix and submit-time snapshots, the remaining tear was a zero header, with no guest
  write to submitted command memory (`BB_CMD_SNAPSHOT_CHECK` clean).
- The tears cluster at loads, when the game's command chunks fill and chain.
- Linux and Windows run the same parser. A stale tail there executes old packets and is rarely
  fatal, which is why it went unnoticed.

## Fix

The graphics, constant-engine and compute parsers end the current buffer after a chained indirect
buffer (`liverpool.cpp`). A chain packet directly in a compute ring has no return target to model,
so it is a hard error. The submit-time snapshot walk stops at a chain as well (`cmd_snapshot.cpp`).
The first chained IBs of a run are logged (`Chained IB #n ... skipping k dwords after it`).
`tests/test_cmd_snapshot.cpp` covers the walk.

The fix is Apple-only (`#ifdef __APPLE__`) until it can be tested on Linux and Windows. The change is
the same everywhere and worth offering upstream.

Related: `docs/macos/gpu-idle.md` (the guest released early) and
`docs/macos/command-snapshots.md` (submit-time copies). Each of the three closed a different way of
parsing words the PS4 command processor would not have executed.
