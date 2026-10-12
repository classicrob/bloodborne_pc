# Heavy jobs on the shared Mac

Eight agents share one M4 Max. Unlimited parallel builds kept the load at 40 to 100: a 3-minute
GPU build took 8 to 11 minutes and no timing meant anything. `tools/macos/heavy.sh` limits heavy
work machine-wide.

```sh
tools/macos/heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive] LABEL -- COMMAND [ARG...]
tools/macos/heavy.sh status
```

| Pool | Slots | For |
|---|---|---|
| `build` | 1 (`BB_HEAVY_BUILD_SLOTS`) + 1 priority-only | builds, test suites, conformance runs, benchmarks, standalone GPU probes; `--priority` (shared builds) may also take the reserved slot |
| `playtest` | 2 (`BB_HEAVY_PLAYTEST_SLOTS`) | game runs through play_route.py: parallel correctness runs, each with its own user dir and window |
| `quick` | 2 (`BB_HEAVY_QUICK_SLOTS`) | short jobs: a one-TU compile check, a single test target built and run **when the library it links is already built in that tree**, a driver-only build. Never a full GPU build: a test target in a cold tree first rebuilds libbbgpu (over 600 s), and the quick pool kills it; use the build pool for that (`--priority` for P0) |
| `sweep` | 1 (`BB_HEAVY_SWEEP_SLOTS`) | the long shader-sweep phase, as its own heavy.sh call after the build (see below) |

A quick job gets a parallelism hint (`BB_BUILD_JOBS=4`, `CMAKE_BUILD_PARALLEL_LEVEL=4`,
`MAKEFLAGS=-j4`; honour it). It prints a warning after 5 min (`BB_HEAVY_QUICK_WARN`). After
10 min (`BB_HEAVY_QUICK_LIMIT`) it is killed with its whole process group, the kill is reported
on stderr, and it exits 124. A job that can take that long belongs in the build pool. The quick
pool exists so one-minute jobs don't wait behind fifteen-minute builds.

To run one test, don't write a runner: `build.sh --test-one <target or script>` builds the
library and probe as usual, then only that test target, and runs each `--test` command naming
it with the same environment (`BB_GPU_USER_DIR`, `VK_DRIVER_FILES`, `BB_GPU_BUILD`, ...),
signing and directories, plus the checks of its outputs. Each run prints `test-one: exit N`, and
the exit status is the first failure's. A name no test command has fails and lists them. In a
tree whose library is built it fits the quick pool:
`tools/macos/heavy.sh --pool quick LABEL -- bash build.sh --test-one capture-quiesce-test`.

That is at most 4 translated (Rosetta) processes from game runs and one build-pool test, which is
the team limit. Parallel runs are for correctness only; their FPS numbers mean nothing.

## Long jobs: build, then sweep

A slot is held by the whole job: its locked descriptor is inherited by every process the job
starts, and heavy.sh cannot hand it back halfway. A job that builds for 20 minutes and then
sweeps shaders for 2 hours therefore holds the only build slot for both. Run the two as separate
calls instead:

```sh
tools/macos/heavy.sh "sweep build" -- <build and test the sweep driver>
tools/macos/heavy.sh --pool sweep "sweep" -- <run the sweep>
```

A sweep started inside a build job runs in that build slot and heavy.sh reports it on stderr.
Sweep jobs get the same scheduling as builds (background QoS, capped parallelism), so the extra
lane adds no load on the performance cores.

## Sharing the Mac with the user

The user works on this Mac while the team runs. Heavy jobs yield to them by default:

| Jobs | Scheduling |
|---|---|
| build, quick and sweep pools (builds, tests, sweeps) | background QoS (`taskpolicy -b`: lowest CPU priority, efficiency cores, throttled I/O) and compile parallelism capped at `BB_HEAVY_BUILD_JOBS` (8) through `BB_BUILD_JOBS`, `CMAKE_BUILD_PARALLEL_LEVEL` and `MAKEFLAGS` (a lower hint is kept) |
| `--priority` (P0 previews, shared builds) | nice 5 and a utility QoS clamp, parallelism capped as above (background QoS made a preview take 37 min) |
| playtest pool (correctness games) | nice 5 and a utility QoS clamp |
| `--gpu exclusive` (perf_run, timing) | unchanged: the user approved perf runs separately |

**When the user is away** (no keyboard or mouse input for `BB_HEAVY_IDLE_MINUTES`, 10, read
from IOHIDSystem's HIDIdleTime), the build pool admits a second job (slot `build-idle`), and
new build, quick and sweep jobs start at nice 5 with a utility QoS clamp (performance cores
allowed; the parallelism cap stays). Jobs already running keep what they started with. Each
job prints its decision ("user idle 75 min: nice 5, utility QoS, 8 jobs" or "user active (idle
2 min): background QoS, 8 jobs"), and the history records it. An unreadable idle time counts as
active and says so.

It is applied once, when the slot is taken; nested calls inherit it. Builds take longer this
way, by the user's choice. `BB_HEAVY_POLITE=0` turns it off (perf or shared builds while the
user is away). Plain `ninja` ignores the parallelism hint: pass `-j "$BB_BUILD_JOBS"`.
The history records each job's scheduling.

## The GPU lock

GPU timings taken while a game runs measure the game too (a contended `--test` once failed the
timestamp gate). So the GPU is its own lock, separate from the pools:

- Games hold it **shared**: a playtest-pool job takes it shared by default.
- A GPU-timing job asks for it **exclusive** (`--gpu exclusive`) and waits until no game runs.
  `tools/macos/perf_run.sh` does this for the whole run (its game runs under the hold), and
  `build.sh --test` does it around the timestamp gate. Short timing jobs need no hand-called quiet
  window.
- While an exclusive request waits it holds `gpu-gate`, which every shared request passes first,
  so games started meanwhile wait behind it instead of starving it.
- `--gpu` works inside a slot too. Under an exclusive hold (`BB_HEAVY_GPU=exclusive`), nested
  calls run directly; asking for exclusive inside a shared hold is an error (it would wait for
  itself).
- `heavy.sh status` prints `gpu: free|shared|exclusive`; each GPU acquisition is in the history.

`--cpu-quiet` (with `--gpu exclusive`, outside any slot; `perf_run.sh` uses it) also keeps the
CPU quiet for the run: CPU load from builds slowed a perf run by 19% even at low priority. A
forked watcher holds `BB_HEAVY_DIR/cpu-quiet`. It pauses every running build, quick and sweep job
(SIGSTOP on its whole process tree; for a quick job, the command under its supervisor, whose
time limit pauses too), and no new one is admitted. When the run ends it resumes them (SIGCONT).
The pause is bounded by `BB_HEAVY_QUIET_MAX` (1200 s); past that the jobs resume with a loud
"CPU QUIET BOUND REACHED". Each pause and resume is printed in the run's output and recorded in
the history. Games are not paused (the exclusive GPU hold already keeps them off).

A paused job says so when it ends, with its exit status, and its end event in the history
carries `paused_s` and `paused_by`:

    heavy: PREVIEW-26 full --test: PAUSED 712 s for perf runs (R0 perf); ended with exit 2: a
    wall-clock timeout in the job may have fired on resume; rerun before calling it a failure

A test's own `alarm()` kept counting through the stop and fired as soon as it resumed
(PREVIEW-25a, the fp16 probe). Our tests use `test_alarm()` from `tests/test_alarm.h` instead:
a forked watchdog that is paused with the test and counts at most half a second per step, so a
pause does not use up the limit. A failure in a paused job gets one rerun, and both results are
reported.

- Slots are flocks under `~/bbport-deps/locks` (`BB_HEAVY_DIR`). A job waits for a free slot of
  its pool and prints who holds them every 30 s.
- The slot's locked descriptor is inherited by the command and everything it starts, and the
  kernel releases it when the last of them exits or is killed. There are no leases or stale locks.
- Inside a slot (`BB_HEAVY_HELD` is set), nested calls run directly.
- Every slot acquisition is appended to `~/bbport-deps/locks/history.jsonl`: label, pool,
  slot, seconds waited, load average, which slots were held, the scheduling, the command and the
  directory it ran in. Every job also gets an end event: exit status, or the signal that killed
  it, and its duration. For that, heavy.sh runs the command as its child and forwards
  SIGINT/SIGTERM/SIGHUP to it. A launcher that needs the job's pid to be the command's sets
  `BB_HEAVY_EXEC=1` and records the end itself (play_route.py: the game, in exit.json).
- **One build/test job per worktree.** A build, quick or sweep job takes its git worktree's lock;
  a second one there is refused at once, naming the holder. Games (the playtest pool, run from
  copied artifacts) may share a worktree; `BB_HEAVY_SHARED_TREE=1` lets another read-only job
  share it too. Never switch or edit a worktree's checkout while a job runs in it.
- **INVALID results.** The start event records the worktree's HEAD and whether it has tracked
  changes. At the end, if HEAD moved or tracked files were edited during the run, heavy.sh prints
  "INVALID: …" and records it in the end event. A job that succeeded then exits 125, not 0, so no
  caller takes it for a pass; a failing job keeps its own status. Untracked files (logs, out/)
  and submodule content (build.sh patches FSR) do not count.
- To stop a job, signal its heavy.sh pid with SIGTERM: it ends the command and records it.
  SIGKILL to heavy.sh alone leaves the command running and holding the slot; `heavy.sh status`
  then shows it as ORPHANED with the command's pid. Never kill by command-line pattern: it hits
  other agents' jobs.

Automatic: on macOS `build.sh` (the GPU build and `--test`) takes a build slot, a priority one
with `BB_HEAVY_PRIORITY=1`. `play_route.py` runs each game in a playtest slot, and time spent
waiting for it does not count against the 60 s startup limit. Everything else heavy must be
started through heavy.sh. `tests/test_heavy.sh` covers the pools, the quick hints and time
limit (group kill, exit 124), waiting, the priority slot, the GPU lock (shared games, an
exclusive job waiting for them, a game started meanwhile waiting behind it, nesting under
exclusive, exclusive-in-shared refused), release on SIGKILL, nesting, exit status and usage
errors.
