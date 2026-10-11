# Heavy jobs on the shared Mac

Eight agents share one M4 Max. Unlimited parallel builds kept the load at 40 to 100: a 3-minute
GPU build took 8 to 11 minutes and no timing meant anything. `tools/macos/heavy.sh` limits heavy
work machine-wide.

```sh
tools/macos/heavy.sh [--pool build|playtest|quick] [--priority] LABEL -- COMMAND [ARG...]
tools/macos/heavy.sh status
```

| Pool | Slots | For |
|---|---|---|
| `build` | 1 (`BB_HEAVY_BUILD_SLOTS`) + 1 priority-only | builds, test suites, conformance runs, benchmarks, standalone GPU probes; `--priority` (shared builds) may also take the reserved slot |
| `playtest` | 3 (`BB_HEAVY_PLAYTEST_SLOTS`) | game runs through play_route.py: parallel correctness runs, each with its own user dir and window |
| `quick` | 2 (`BB_HEAVY_QUICK_SLOTS`) | short jobs: a one-TU compile check, a single test link or run, a driver-only build, a sweep step. Never a full GPU build |

A quick job gets a parallelism hint (`BB_BUILD_JOBS=4`, `CMAKE_BUILD_PARALLEL_LEVEL=4`,
`MAKEFLAGS=-j4`; honour it). It prints a warning after 5 min (`BB_HEAVY_QUICK_WARN`). After
10 min (`BB_HEAVY_QUICK_LIMIT`) it is killed with its whole process group, the kill is reported
on stderr, and it exits 124. A job that can take that long belongs in the build pool. The quick
pool exists so one-minute jobs don't wait behind fifteen-minute builds.

That is at most 4 translated (Rosetta) processes from game runs and one build-pool test, which is
the team limit. Perf runs stay alone, in quiet windows. Parallel runs are for correctness only;
their FPS numbers mean nothing.

- Slots are flocks under `~/bbport-deps/locks` (`BB_HEAVY_DIR`). A job waits for a free slot of
  its pool and prints who holds them every 30 s.
- The slot's locked descriptor is inherited by the command and everything it starts, and the
  kernel releases it when the last of them exits or is killed. There are no leases or stale locks.
- Inside a slot (`BB_HEAVY_HELD` is set), nested calls run directly.
- Every slot acquisition is appended to `~/bbport-deps/locks/history.jsonl`: label, pool,
  slot, seconds waited, load average, and which slots were held. Quick jobs also record their
  duration and exit status. This is the data for sizing the pools.

Automatic: on macOS `build.sh` (the GPU build and `--test`) takes a build slot, a priority one
with `BB_HEAVY_PRIORITY=1`. `play_route.py` runs each game in a playtest slot, and time spent
waiting for it does not count against the 60 s startup limit. Everything else heavy must be
started through heavy.sh. `tests/test_heavy.sh` covers the pools, the quick hints and time
limit (group kill, exit 124), waiting, the priority slot, release on SIGKILL, nesting, exit
status and usage errors.
