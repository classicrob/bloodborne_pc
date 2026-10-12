# Hot GPU-thread scheduling: QoS class and task role (diagnostic)

The hot GPU-side threads are stage A (GPU command), stage B (draw recording) and the Vulkan
recording threads. In the verifier's replay they moved from P-cores to E-cores after about
1-1.5 s of sustained load as an accessory app (task role 6). A regular app (role 2) stayed on
P-cores but slowed as well, with falling clocks and rising GPU time, so the cause is probably an
SoC power budget (Low Power Mode is under investigation). The two settings below are diagnostic
A/B arms. Neither is on by default.

Both are read once from the environment (gpu/shim/bbport_hot_thread_qos.h):
- `BB_HOT_THREAD_QOS=user-interactive|user-initiated|default` sets the thread's QoS class at the
  start of each hot thread.
- `BB_TASK_ROLE=foreground|default-app` sets the process's task role (TASK_CATEGORY_POLICY). It
  is applied once by bb-probe's main thread, before bb_macos_ui_run starts the worker that runs
  the game (bbgpu_apply_task_role). Liverpool's constructor, which runs on that worker, checks
  that the role is in effect before it starts stage A.

Unset or empty leaves today's behaviour. Any other value stops, and so do a refused set, a role
applied off the main thread, and a role not in effect at Liverpool's constructor.

Each hot thread prints one startup line. It gives the class requested, the Mach priorities
(current, base, max), the policy and the task role, then both settings. The base priority is the
effective tier. macOS cannot report a thread's core cluster.

## Measured (2026-10-12)

**Through bb-probe's startup path.** In label-check's UI-worker fixture, main applies the role,
then bb_macos_ui_run's worker starts "stage A". Stage A's base priority:

| stage A's class | role foreground | role as launched (0) |
|---|---|---|
| user-interactive | 47 | 31 |
| user-initiated | 37 | 31 |
| default | 31 | 31 |

So the role lifts the QoS ceiling and the class sets the tier. **Neither alone moves a hot
thread.** The arm to try is `BB_TASK_ROLE=foreground BB_HOT_THREAD_QOS=user-interactive`. With
the role set, the main thread itself reports user-interactive at 47.

Also measured:
- **heavy.sh's clamp caps everything.** Playtest, build and quick jobs run under
  `taskpolicy -c utility` (or `-b`), where every case ran at 20. Perf runs (gpu exclusive) are
  not clamped. Run a scheduling A/B unclamped.
- **A process may set its own role.** It needs no entitlement, and the role survives exec
  (probe).
- **Set it from the main thread.** In probes, a role set by any other thread left that thread,
  and the threads it then created, at 31 for 3 s and more. A forked child's threads were never
  lifted. Early probes that set a class on threads created straight from the main thread read 47
  for every class in role 1; the fixture above, which follows the real path, is the measurement
  to trust.
- **Tests.** label-check checks the values, and each stop by its message in fresh exec'd
  children. It checks the startup line, and the UI-worker path with both settings: stage A is
  above as-launched, and the class alone is not. When the process is clamped, the priority
  checks print SKIPPED.
