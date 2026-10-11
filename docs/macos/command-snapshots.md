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
- **Stream writes parsed before the submit:** the parser hands `WRITE_DATA` and `DMA_DATA` memory
  writes to the draw recording thread, which runs them later. A buffer the guest submits after the
  packet was parsed may still hold the old words (games patch command chunks this way; zeros or a
  stale chunk tore as "Unimplemented PM4 type 0"). The PS4 command processor executes the write
  before it fetches that buffer. So every copy first waits until no such write overlaps it
  (`AmdGpu::StreamWrites`: registered when parsed, released when the write has run), and the walk
  for indirect buffers sees the final words. If one of them was recorded as a GPU copy or fill, it
  reaches memory only when the GPU runs it, and that is a hard error (not implemented). Waits are
  logged (first 8, then every 1024th) and counted in the 600-frame summary.
- **Stream writes parsed after the submit:** a `WRITE_DATA`/`DMA_DATA` into command memory of a
  submission not yet parsed would be seen by the PS4 command processor but not by the copy, so it is
  a hard error naming the packet and submission.
- **`BB_CMD_SNAPSHOT_CHECK=1` (diagnostic):** when a submission's parsing starts, its copies are
  compared with live memory, and guest writes made after submit are logged as `CMD-SNAPSHOT ...`. It cannot see a stream write still on its way: live memory is as stale as the copy.
- **Cost:** the "Parse lag" summary every 600 frames reports snapshot KiB per frame and the mean
  and maximum microseconds per submit. `BB_COPY_GPU_BUFFERS` is refused on Apple.

Test: `tests/test_cmd_snapshot.cpp` (nested and constant IBs, an IB called twice, an unreadable
target, later guest writes, unparsed ranges, truncated streams, pending stream writes into a called
chunk and into the submitted buffer, a pending write elsewhere, a GPU-side DMA into submitted
command memory).
