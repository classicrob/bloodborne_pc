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
  At submit it also logs `CMD-OVERLAP` when a captured buffer partly overlaps one submitted in the
  last 64 submissions (two command buffers written into the same memory), with the guest thread
  numbers the builder trace uses. The builder trace logs `CMD-INTERLEAVE` when two threads advance
  one command stream: thread A writes, thread B writes at or after that, then A writes on at or
  after B. This is a shared command buffer pointer, as at movie start, where main draws into the
  render thread's context. Each line gives both threads, rdtsc stamps (the HLE trace's clock) and
  guest call chains. Calls from other threads in between, such as workers building their own
  buffers, are ignored. One theoretical false positive: A's buffer ends just below B's chunk, and
  A's next chunk starts just above B's write (all within 4096 dwords). Both chains are in the
  report so this case can be checked.
- **Cost:** the "Parse lag" summary every 600 frames reports snapshot KiB per frame and the mean
  and maximum microseconds per submit. `BB_COPY_GPU_BUFFERS` is refused on Apple.

Test: `tests/test_cmd_snapshot.cpp` (nested and constant IBs, an IB called twice, an unreadable
target, later guest writes, unparsed ranges, truncated streams, pending stream writes into a called
chunk and into the submitted buffer, a pending write elsewhere, a GPU-side DMA into submitted
command memory).

## Guest data read from command memory (`BB_CMD_DATA_CHECK=1`, diagnostic)

Gnm can allocate descriptor tables and constants inside a command buffer's chunk. The engine
allocates them downward from the chunk's end (0x26b07f0, Astra-2), so they lie **outside** the
submitted `[begin, cmdptr)` range that the snapshot copies. The guest may reuse a chunk once it
believes the GPU is done with it. If we then read such data at parse time, we read bytes the
guest has already replaced: for example, the GXFfxLightShaft vertex-buffer table read as floats.

`BB_CMD_DATA_CHECK=1` (`gpu/shim/bbport_cmd_data.h`) locates these parse-time guest reads
relative to the snapshots the GPU thread still holds:
- `Info::ReadUdReg`: a descriptor table read through a user-data pointer, including the fetch
  shader's vertex-buffer table;
- `Info::ReadTessConstantBuffer`.

The classifier (`BbCmdData::Classify`) is pure. It runs under the snapshot lock against the
snapshots' own copies and reads no guest memory. Each read falls into one of these:
- **Inside:** every held region containing the range is compared with the bytes just read.
  - The result says how many held copies contain it and how many differ. It also says whether
    one of them is the parser's current submission (recorded per packet on the GPU thread; reads
    on the draw thread have none).
  - The selected region is the current submission's, else the newest. The line says first
    whether the **selected** copy matches the read, then how many other held copies differ. This
    is a byte comparison, not an order: it does not say which copy was written last.
  - A region not copied at submit (unreadable then; a valid stream never executes it) has no
    copy of its own. It is located and named "not copied at submit", never compared.
  - A resubmitted chunk shows up as several copies. A difference shows that a held copy differs
    from the read, not by itself which submission's bytes the PS4 would have used.
- **Straddling** a held region's boundary, when no held region contains the range (a containing
  region always wins).
- **A candidate chunk tail:** outside every held region, but in the same chunk of a known pool
  as a held region that ends below it. The newest such submission is named.
  - Pools come from `BB_CMD_DATA_POOLS` (`base:chunks,...`). The default is Astra-2's captured A1
    at 0x7039400010 (128 chunks) and A2 at 0x703b400020 (384 chunks).
  - `BB_CMD_DATA_CHUNK` is the chunk size, default 256 KiB.
  - The geometry is assumed, and ownership of the address is not established.
  - After the lock is released, the chunk's status word (its last 8 bytes: 0 free, 1 claimed,
    2 released) is read without faulting (Mach). If that fails it is reported as unreadable.
- **Not located:** none of the above. The address may still be in a known pool (a chunk with no
  held region below it, or a range crossing a chunk end). Parsed snapshots are released, so this
  never means outside command memory.

**Output** goes to stdout:
- `CMD-DATA #n` lines for the first 16 notable reads: the selected copy differs, straddling, or
  a candidate tail;
- a summary every 65536 reads across all kinds, with counts per read kind and shader hash.

The `CheckNumberConversion` stop appends this thread's latest instrumented read for the descriptor
that stopped, **as observed then** (it never re-reads guest memory). The latest read is forgotten
before each descriptor is read, so when that descriptor came from direct user data or the
uninstrumented flat buffer, the stop says so instead of naming an earlier read.

The stop line also says:
- **where the table pointer came from (with `BB_CMD_DATA_CHECK=1`):**
  - The SET_SH_REG that last wrote its low user-data word: the graphics stream or a compute
    queue, the packet's guest address, its submission, and how many flip packets the parser had
    passed between that write and the read.
  - The high word too, when it was written separately.
  - Or: that no tracked SET_SH_REG wrote it since the check started; or that the word is not a
    tracked graphics user-data register (a host snapshot such as stage B or draw preparation,
    compute queue state, or a spilled slot; not by itself proof of a spill).
  - "An EARLIER frame" makes retained state a candidate, not a defect: the game may keep a
    pointer, and a current SET can also republish a stale one (Astra-2).
  - The read's frame sample is taken under the lock origins are recorded under. An origin
    newer than it is printed as such, not as an age.
  - Our newest observed DUMP_CONST_RAM overlapping the table bytes (of the last 512). It is not
    necessarily the bytes' last writer: CPU and other writes are not tracked.
  - Liverpool records SET_SH_REG writes and dumps only while the check is on.
- **compute-queue SET_SH_REG writes outside the compute program, always.**
  - The parser applies them to the one shared register file, as upstream does, i.e. into the
    graphics SH registers below 0x200. On the PS4 a compute ring has its own registers.
  - They are counted in total, into graphics user data, and above the compute program, with the
    latest write into graphics user data (queue, SH offset, words, first value).
  - The first 8 of each kind are logged as they happen, and a "Compute SET_SH_REG" summary line
    is printed every 600 frame ends once any occurred.
- **for a descriptor from the flattened buffer (with `BB_CMD_DATA_CHECK=1`)**, where nothing
  read is instrumented, because the SRT walker reads tables through user-data pointers:
  - the source of each descriptor dword (immediate, user data sN, or walker dword k) and s0..s15;
  - each user-data word's last tracked SET_SH_REG writer and its age in flips;
  - for each pointer-like pair (a 48-bit address), the mapping holding it now.
  The walker's own table offsets come from the shader (Sol-2's traces); for vs 0x9da6aacb the
  bad descriptor is walker dwords 36..39 = s[6:7] + dwords 0..3.
- **where the read lies at the stop:** the guest mapping holding it, `[start, end)` and size, or
  the unmapped gap, and how many other guest mappings view the same backing. These come from
  runtime_memory's mapping table, a query and not a guest memory read. Stop hunt #1 found every
  table read outside command memory (20261011-192653-1080p-HUNT-1), so the mapping is the
  lead.
- **how far each side of the flip pipeline had got, always (even without `BB_CMD_DATA_CHECK`).**
  These come from gpu/shim/bbport_frame_marks.h:
  - guest flip API calls, counted at entry, before admission;
  - flip packets parsed by the GPU command thread;
  - flip preparation entered (on the draw recording thread for EOP flips);
  - flip status published to the guest.

  Each is a count, with the latest argument where known. These are approximate observations,
  each sampled on its own. Judged against the guest's reuse gate, they show whether the guest
  could already reuse the memory the parser was reading.

**Limits:**
- Supporting evidence only.
- Reads made by generated code (the SRT walker) are not instrumented.
- Each lookup takes the snapshot lock and scans the held regions. That serialises against
  submissions and can change the timing of the race it observes.
- Numeric settings are parsed strictly (decimal without a leading zero, or 0x hex; no
  whitespace or sign); a malformed value stops.
- Lifetime: each lookup holds a registry lock (shared) while it uses the Liverpool owner.
  `~Liverpool` unregisters first, holding none of its own locks, so it waits out any lookup in
  flight on any thread; later reads are not located.
