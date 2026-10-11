# AvPlayer frame polling versus Stop

The ca56783 movie-skip crash in `AvPlayerSource::GetAudioData` dereferenced an
empty optional frame. `AvPlayerState` deliberately holds a shared source-lifetime
lock for Stop and getters; that prevents source replacement, but does not serialize
queue draining. Source::Stop held `m_state_mutex`; getters did not. Audio used
Size/Pop and video Size/Front/Pop. Additionally, AvPlayerQueue::Size read its
std::queue without locking, and Pop checked emptiness before taking the lock.

The correction is shared across platforms: queue size and conditional dequeue
are locked, no queue element reference escapes the queue lock, and frame getters
hold the existing source state mutex through selection, old-buffer recycling,
clock updates and output assignment. Video selection preserves timestamp gating:
a future frame remains queued. Empty, paused or stopped requests return false
and leave output untouched, preserving the existing boolean no-data API. This is
not a new PS4 error-code interpretation or evidence from console hardware.

Pause, Resume, CurrentTime and IsActive use the same source lock. Internal locked
helpers avoid recursive locking. Start/Stop already own that lock; decoder workers
do not take it, so Stop can request cancellation and join them while holding it.
Stream count/info read metadata that Stop does not change; HasFrames uses only the
now-locked queue size and atomic EOF flag; SetLooping uses an atomic. State's unique
source lock excludes all its getters when replacing/destroying the source.

Limits: the outer sceAvPlayerClose still takes a raw handle and deletes it. This
unit does not establish that a caller may enter an API after Close has destroyed
that handle, or retain frame bytes across Stop/another successful frame call.
Event ordering and the guest movie resource faults remain separate investigations;
the EINVAL panic has not been proven to share this cause.

`tests/test_avplayer_lifecycle_macos.sh` compiles actual AvPlayer/FFmpeg and the
actual Mach memory/TLS/low-stack backend, signs it, and runs under Rosetta. It
retains natural EOF, full-output-queue cancellation, repeated/concurrent Stop and
destruction checks. Thirty-two additional sessions race low-stack audio and video
pollers (including clock/activity reads) with Stop. After Stop, both frame forms
return false with untouched output, clock=0 and inactive state; every allocated
guest buffer is freed once. Passed on macOS 27.2. The test has a 45-second alarm.

Cost: one source mutex acquisition per public poll/clock query, and one queue
mutex per size/dequeue. No per-draw renderer work, copying, or GPU synchronization
was added. This fixes C++ data races on all platforms.
