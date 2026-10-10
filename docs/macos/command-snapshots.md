# Submit-time command snapshots (macOS)

The GPU command thread parses a graphics submission some time after the guest submits it. With the
GpuIdle fix (`docs/macos/gpu-idle.md`) the parser still read command memory the guest had changed
after submitting it: a production run on 8cf9ed7 stopped with `Unimplemented PM4 type 0, base reg 0,
size 1` (a zeroed packet) with the parser only about 2 submissions behind. On the PS4 the command
processor fetches a submitted buffer long before the guest may legally reuse it. So on Apple the
parser reads copies taken at submit (`AmdGpu::CmdSnapshot`, `Liverpool::SubmitGfx`).

- **What is copied:** the top-level DCB and CCB, plus every indirect buffer they reach, walked
  recursively at submit (`IT_INDIRECT_BUFFER` from DCB streams, `IT_INDIRECT_BUFFER_CONST` from CCB
  streams). Each buffer is copied once. The arena is per submission and grows as needed.
- **Unreadable targets:** an IB target that cannot be read at submit (a stream can name IBs inside
  regions it skips) is recorded as not copied. Executing it is a hard error.
- **REWIND:** means "refetch from memory", so the rest of that buffer and the IBs it reaches are
  parsed from live guest memory.
- **Stream writes:** a `WRITE_DATA`/`DMA_DATA` into command memory of a submission not yet parsed
  would be seen by the PS4 command processor but not by the copy, so it is a hard error naming the
  packet and submission.
- **`BB_CMD_SNAPSHOT_CHECK=1` (diagnostic):** when a submission's parsing starts, its copies are
  compared with live memory, and guest writes made after submit are logged as `CMD-SNAPSHOT ...`.
- **Cost:** the "Parse lag" summary every 600 frames reports snapshot KiB per frame and the mean
  and maximum microseconds per submit. `BB_COPY_GPU_BUFFERS` is refused on Apple.

Test: `tests/test_cmd_snapshot.cpp` (nested and constant IBs, an IB called twice, an unreadable
target, later guest writes, unparsed ranges, truncated streams).
