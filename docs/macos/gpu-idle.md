# GpuIdle must mean idle

## Mechanism

`sceGnmSubmitDone` sets `submission_lock` unless the GPU is idle; the guest's next submission
waits for it (`WaitGpuIdle` in gnmdriver.cpp). The lock is cleared by the GpuIdle interrupt
(`ResetSubmissionLock`). Bloodborne double-buffers its command buffers: once its submit for frame
N+1 goes through, it treats frame N-1's buffers as consumed and rewrites them.

`Liverpool::Process` raises GpuIdle at the end of every loop iteration. Before raising it, the
iteration drains the draw recording thread and waits for the fences that thread deferred, which can
take a long time while that thread lags behind (loading, pipeline compiles). If the guest submits and
calls `sceGnmSubmitDone` during that drain, the interrupt raised afterwards is stale: it clears the
new lock while the new submissions are still queued. The guest is released a frame early and
rewrites buffers the parser has not reached. Those packets are torn (`NextPacket: packet length
exceeds remaining submission size`), and eventually the parser fails with `Wrong PM4 type 0/1`.

How this explains the production runs (2026-10-10, Opus-Playtest):

- **Canary:** caught exactly this. A submitted top-level DCB had 4 words rewritten by the guest CPU
  about 9 submissions (one frame) before it was parsed.
- **Default config:** torn every time (6/6).
- **`PipelinedMemoryWrites` off:** clean 2/2. Its drains happen during parsing, so the final drain
  is short and the stale window nearly closes.
- **All pipelining off:** no draw pipe, so the drain takes no time.
- **Write-ordering fix (9e79075, reverted):** zero syncs and still torn. Stream writes into command
  memory were not the cause.

## Fix (Apple)

GpuIdle is decided and raised under `submit_mutex`, the lock submissions take, and only when no
submission is queued. Otherwise the loop processes the new submissions and raises GpuIdle once they
are retired, which is the PS4 meaning of the interrupt. Each held-back interrupt is logged (first 8 per
window). Every 600 frames, a summary logs the parse lag (graphics submissions queued ahead of the
parser at each submit: mean and max) and how often GpuIdle was held back. Linux and Windows have
the same code and the same race, with a shorter drain; the fix is Apple-only until it can be tested
there.

## Not the cause (kept for the record)

- **Submit-time snapshots of command memory:** not needed once the guest is no longer released
  early.
- **Stage A reading command memory or `COND_EXEC` predicates before stage B's `WRITE_DATA`/`DMA_DATA`
  land:** this ordering gap is real in principle, but no such write occurred at the tear points
  (9e79075 counted zero).
