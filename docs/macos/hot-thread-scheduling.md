# Hot GPU-thread scheduling: QoS class and task role (diagnostic)

The hot GPU-side threads are stage A (GPU command), stage B (draw recording) and the Vulkan
recording threads. In the verifier's replay they moved from P-cores to E-cores after about
1-1.5 s of sustained load. That happened when the process was an accessory app (task role 6); a
regular app (role 2) stayed on P-cores. The fix for test runs is a regular-app window mode
(Renderer). The two settings below are diagnostic A/B arms. Neither is on by default.

Both are read once from the environment (gpu/shim/bbport_hot_thread_qos.h):
- `BB_HOT_THREAD_QOS=user-interactive|user-initiated|default` sets the thread's QoS class at the
  start of each hot thread.
- `BB_TASK_ROLE=foreground|default-app` sets the process's task role (TASK_CATEGORY_POLICY). It
  is applied once, in Liverpool's constructor, on the main thread, before the hot threads exist.

Unset or empty leaves today's behaviour. Any other value stops, and so does a refused set or a
role applied off the main thread.

Each hot thread prints one startup line. It gives the class requested, the Mach priorities
(current, base, max), the policy and the task role, then both settings. The base priority is the
effective tier. macOS cannot report a thread's core cluster.

## Measured (label-check, and small probes native and under Rosetta, 2026-10-12)

- **The role sets the base priority; the requested class did not.** As launched from our shells
  (role 0), user-interactive, user-initiated, default and utility all ran at base 31. With role
  `default-app` (7) every class ran at 46; with `foreground` (1), at 47.
- **heavy.sh's clamp caps everything.** Playtest, build and quick jobs run under
  `taskpolicy -c utility` (or `-b`), where every class and role ran at 20. Perf runs (gpu
  exclusive) are not clamped. Run a scheduling A/B unclamped.
- **A process may set its own role.** It needs no entitlement, and the role survives exec.
- **Set it from the main thread.** Set from the main thread (directly or through a joined
  helper), the main thread and threads created afterwards run at the role's priority. Set by any
  other thread, that thread and every thread it then creates stay at 31 (3 s and more). A forked
  child's threads are never lifted.
- **Tests.** label-check checks the values, the loud stops, the startup line, and the role
  applied from the main thread lifting the main thread and a new thread. When the process is
  clamped, the priority checks print SKIPPED.
