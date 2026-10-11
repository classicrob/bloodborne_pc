# Movie events and state lifetime audit

The four observed failures remain distinct evidence: an empty host audio-frame
optional during skip (fixed by 8192d12), a null guest vertex-shader wrapper at
0x26c01c3, a null guest resource-index table at 0x26ae211, and a guest audio-worker
mutex EINVAL panic. A common premature-lifetime transition remains a hypothesis,
not an established cause.

## What Bloodborne actually does

Static inspection of the read-only EU v1.09 eboot (same hash as
[movie-resource-index-fault.md](movie-resource-index-fault.md)) shows:

- At 0x28fdedc/0x28fdeeb, the movie initializer fills the AvPlayer callback object
  with the movie pointer and callback 0x28fe290. At 0x28fdf1a it writes auto_start=0.
- Callback 0x28fe290 constructs a 16-byte event (ID, source ID, payload pointer)
  and enqueues it in movie+0x48 via 0x2900ae0. The enqueue takes the guest queue's
  lock via its vtable and copies the event. It does not directly free the movie
  or graphics resources. This makes callback thread affinity alone a weak causal
  explanation; callback/event *ordering and lifetime* still matter.
- Consumers 0x28fe620 and 0x28fe6a0 pop that queue. ID 1 clears movie+0x2db, ID 3
  sets it, and ID 2 invokes setup 0x28ff6a0. These match Stop, Play and Ready.
  Setup builds the embedded movie shader and related resources, and writes movie
  state +8 from 3 (in progress) to 2 only after successful setup.
- The movie start wrapper at 0x28fe750 calls AvPlayer only when the handle is
  present and movie state +8 is 2. Thus the decoded normal path does not knowingly
  start decoding before its Ready-driven graphics setup finishes.
- Cleanup 0x28fe2e0 frees and clears movie+0xe0 before Close at 0x28fe5d3. Therefore
  absence of a Close log cannot by itself exclude concurrent guest cleanup.

In Playtest's `20261010-211057-1080p-ACCEPT5-4-ca56783/game.log`, lines 4246–4260
show Ready, Play, Buffering, Play. The 0x26c01c3/0xd2 fault follows at movie start,
without Stop, EOF or decoder exit. A hypothesis involving only premature EOF/Stop
cannot explain that particular trace.

## Current host delivery

| Event path | Thread | Relevant ordering |
|---|---|---|
| Ready after FindStreams | controller | source unique lock released before callback |
| Start/Play, explicit Stop, Pause/Resume | calling thread | callback while holding source lock |
| Buffering and return to Play | controller | state change then callback |
| Natural EOF | demux sets internal EOF state after decoder joins; controller emits Stop | queues drained before notification claim; source lock released before callback |
| Warning | queued for controller | payload copied to low memory on Apple for callback duration |

Comments describing OnPlaybackStateChanged as controller-only are inaccurate;
call sites determine the actual threading above. No callback-thread policy has
been changed without PS4 evidence.

## Confirmed state-machine bugs and bounded repair

Start took an exclusive m_source_mutex and, outside Ready/Stop states, called
public Stop, which tried to acquire the same shared_mutex in shared mode. A
private StopLocked helper now expresses the already-held-lock contract; public
Stop keeps its original shared-lock boundary. Starting without a source returns
failure instead of dereferencing null.

EOF used an atomic exchange to claim its Stop notification, but explicit Stop
unconditionally emitted after storing the flag. EOF and explicit Stop could
therefore emit twice for the same run (even sequentially: EOF notification followed
by explicit Stop). Both now use the same atomic exchange. Start resets the claim
for the next run. Callback thread and existing lock scopes are unchanged.

The regression links actual AvPlayerState/controller, Source, FFmpeg and the
Mach low-stack backend. It restarts while playing/buffering (formerly recursive
lock), checks repeated Stop, then holds the natural-EOF Stop callback open on the
controller while explicit Stop runs on the calling thread. Exactly one Stop event
must occur. A new run gets a new notification claim. Existing source polling/Stop
and allocation-balance tests remain in the same fixture; pollers must each deliver
a real frame before Stop is raced. The signed Rosetta run on macOS 27.2 passed
four controller sessions, 32 poller sessions and the original 12 lifecycle cases
via `tests/test_avplayer_lifecycle_macos.sh`, through the machine job limiter.

## Semantics evidence and remaining limits

Public [OpenOrbis AvPlayer declarations](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/AvPlayer.h)
provide names, not callback ordering/thread/lifetime contracts. The related
[VitaSDK API documentation](https://docs.vitasdk.org/group__SceAvPlayerUser.html)
describes GetAudioData/GetVideoData as boolean new-data queries, but is not PS4
threading evidence. No public console trace or authoritative PS4 contract found
in this audit establishes whether notifications must always use one worker.
The fixed bugs are ordinary C++ locking/duplicate-notification defects; the
repair does not invent a new callback delivery policy.

Still to qualify: raw-handle Close racing entry from another CPU thread, frame
buffer retention after a getter returns concurrently with Stop, and old EOF
callbacks already claimed but not yet delivered when a new source/run begins.
The atomic claim is per current playback run, not a comprehensive generation-
owned event queue. Concurrent source replacement/callback reentrancy also needs
its own contract rather than a speculative broad rewrite.

Next evidence: BB_FAULT_DUMP on the movie route, with
`r9,[r9],[[r9]+0x38],r11,r14+0xb3c0,rbp,rsp,rcx`. Correlate actual resource fields
and saved registers with the existing event log before connecting either guest
null dereference to an AvPlayer state transition. Mutex EINVAL diagnostics must
identify whether the guest destroyed sentinel or the native mutex rejected an
operation; the earlier panic alone did not distinguish them.

## Entry-trace tear: 20261010-222038

Playtest's `20261010-222038-1080p-PAR-ep1-entry/game.log` used the six-site
entry probe (`7faf6831` probe SHA prefix) with the `4e7622b` GPU library
(`d7ee57b6` SHA prefix). It contains 21,615 entry records and a real two-writer
PM4 tear. The second builder thread has an empty pthread name and no backtrace.
Neither property identifies an AvPlayer callback thread:

- `runtime_thread_attach_main` previously changed only the GuestThread name;
  it did not name the pthread. The main guest executes on a switched low stack.
- `guest_backtrace.h` at that build accepts only the native pthread stack range,
  so it rejects both main's switched stack and HLE switched stacks.
- AvPlayer's controller, demuxer and decoder bodies explicitly name their
  pthreads before callback work. An empty name is therefore not affirmative
  evidence for any of those workers.

The entry records provide a more specific result:

| Guest handle | Entries | Identification |
|---|---|---|
| `0x80593c680` | 21,539 out-of-space callbacks; 13 each factory/init-from-parent/initializer/copy/destructor | initial main attachment, allocated at log line 96 before native module initialization |
| `0x805973740` | one each init-from-parent/initializer/copy/destructor | allocation at lines 5334–5336 beside GXRenderThread creation |

All 14 copy assignments occur at startup, lines 5350–5451. The 13 child copy
sources have zero begin/end/cmdptr. The other copy initializes destination
`0x70282a2850` from stack object `0x806b28660`, whose command pointer is
`0x7039440410`. No selected copy-site hit occurs during the movie, and no third
handle occurs at any of these six sites. This rules out the claimed observation
of a movie-time live-chunk copy at this particular function; it does not rule
out another copy path or direct concurrent use of an existing object. The
identity-enabled probe and low-stack-aware builder walk are needed to join the
builder writer to the entry records directly.

### Callback paths checked against source

`GuestBuffer` construction is the only frame-memory allocation callback path.
`AvPlayerSource::Start` allocates the video and audio pools synchronously, before
launching the decoder and demuxer threads. Normal decoder loops pop and recycle
those buffers; they do not invoke allocation callbacks per frame. A buffer
abandoned on an error path can invoke its deallocator on that worker. Stop joins
workers and then drains pools/current frames on the Stop caller.

Auto-start can execute Start on the controller via Ready -> AutoPlay callback.
However, Bloodborne's static initialization above explicitly disables auto-start
and supplies its own event callback. Its normal Start therefore allocates on the
explicit Start caller, after guest Ready processing. Ready/Buffering notifications
still originate on the controller, while explicit Play/Stop originate on their
calling thread. The guest event callback enqueues a record rather than building
GPU commands directly.

File replacement open/size run in source initialization; read callbacks can run
on initialization, controller stream discovery, Start seeking, and demux reads;
close runs on source destruction. This movie log explicitly selects the host
file path at line 942754, so that run does not use guest file callbacks.

The current [upstream shadPS4 controller implementation](https://raw.githubusercontent.com/shadps4-emu/shadPS4/main/src/core/libraries/avplayer/avplayer_state.cpp)
also separates controller auto-start from explicit Start and names the controller
thread. That is implementation corroboration, not a PS4 callback-affinity
contract. No authoritative PS4 threading specification or console measurement
was found that justifies moving these callbacks to another thread. No callback
threading behavior was changed as part of this investigation.
