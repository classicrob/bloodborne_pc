#!/usr/bin/env python3
"""Machine-wide limit on heavy jobs (GPU builds, test suites, game runs, benchmarks).

  tools/macos/heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive [--cpu-quiet]]
                       LABEL -- COMMAND [ARG...]
  tools/macos/heavy.sh status

Pools of slots, each slot an flock on a file in BB_HEAVY_DIR (default ~/bbport-deps/locks):
  build     BB_HEAVY_BUILD_SLOTS (1) for builds, test suites, benchmarks and probes, plus one
            slot that only --priority jobs (shared builds, the critical lane) may take;
  playtest  BB_HEAVY_PLAYTEST_SLOTS (2) for game runs (play_route.py), parallel correctness runs;
  quick     BB_HEAVY_QUICK_SLOTS (2) for jobs that are short by declaration: one-TU compile
            checks, a single test link or run, a driver-only build. A quick job is told to use
            4 jobs (BB_BUILD_JOBS, MAKEFLAGS, CMAKE_BUILD_PARALLEL_LEVEL), warned on stderr after
            BB_HEAVY_QUICK_WARN (300) s, and killed with its whole process group, reported, and
            exit 124 after BB_HEAVY_QUICK_LIMIT (600) s: a long job belongs in the build pool.
            A small test-target build (one target and its run) belongs here only when the library
            it links is already built in that tree: in a cold tree it rebuilds libbbgpu first
            (over 600 s) and is killed; use the build pool for that;
  sweep     BB_HEAVY_SWEEP_SLOTS (1) for the long shader-sweep phase, run as its own heavy.sh
            call after the build (a slot is held by the whole job, so a sweep inside a build job
            keeps the build slot; that is reported). Background QoS like builds, so it does not
            add load on the performance cores.
Each slot acquisition (pool, slot, wait, load average, slots held) is appended to
BB_HEAVY_DIR/history.jsonl; quick jobs also record their duration and exit status.
The GPU is a separate lock (BB_HEAVY_DIR/gpu). Games (the playtest pool) hold it shared; a
GPU-timing job (perf_run.sh, the timestamp gate) asks for it exclusive with --gpu exclusive and
waits until no game runs, so its numbers are never taken on a shared GPU. While an exclusive
request waits it holds BB_HEAVY_DIR/gpu-gate, which every shared request passes first, so new
games wait behind it instead of starving it. --gpu works inside a slot too (build.sh --test
asks for it around the timestamp gate). A command started under an exclusive GPU hold (a game
under perf_run.sh) runs directly; asking for exclusive under a shared hold is an error.
A job takes a slot of its pool, then execs COMMAND with the locked descriptor inherited: the slot
stays held by the job and every process it starts, and the kernel releases it when the last of
them exits or is killed. There is no lease to expire and nothing to clean up. A command started
inside a slot (BB_HEAVY_HELD is set) runs directly, so build.sh under heavy.sh does not wait for
itself.
"""
import ctypes
import ctypes.util
import datetime
import fcntl
import hashlib
import json
import os
import re
import signal
import struct
import subprocess
import sys
import time

WAIT_REPORT_SECONDS = 30


def fail(message, status=2):
    print(f'heavy: {message}', file=sys.stderr)
    sys.exit(status)


def lock_dir():
    path = os.environ.get('BB_HEAVY_DIR') or os.path.expanduser('~/bbport-deps/locks')
    os.makedirs(path, exist_ok=True)
    return path


POOLS = {'build': ('BB_HEAVY_BUILD_SLOTS', 1), 'playtest': ('BB_HEAVY_PLAYTEST_SLOTS', 2),
         'quick': ('BB_HEAVY_QUICK_SLOTS', 2), 'sweep': ('BB_HEAVY_SWEEP_SLOTS', 1)}
QUICK_JOBS = '4'


def slot_names(pool):
    variable, default = POOLS[pool]
    value = os.environ.get(variable, str(default))
    if not value.isdigit() or int(value) < 1:
        fail(f'{variable} must be a positive whole number')
    return [f'{pool}{i}' for i in range(int(value))]


def try_lock(name):
    """An open descriptor holding the slot's lock, or None when another job holds it."""
    fd = os.open(os.path.join(lock_dir(), name), os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except BlockingIOError:
        os.close(fd)
        return None


def holder(name):
    """What the slot file says about its current holder (written after locking)."""
    try:
        with open(os.path.join(lock_dir(), name)) as file:
            return json.loads(file.read() or '{}')
    except (OSError, ValueError):
        return {}


def lock_file(name):
    return os.open(os.path.join(lock_dir(), name), os.O_RDWR | os.O_CREAT, 0o644)


def wait_flock(fd, mode, label, what):
    """Blocks until flock(fd, mode) succeeds, reporting every WAIT_REPORT_SECONDS; returns the wait."""
    started = reported = time.monotonic()
    while True:
        try:
            fcntl.flock(fd, mode | fcntl.LOCK_NB)
            return time.monotonic() - started
        except BlockingIOError:
            pass
        if time.monotonic() - reported >= WAIT_REPORT_SECONDS:
            print(f'heavy: {label}: waiting for {what} ({gpu_state()})', file=sys.stderr, flush=True)
            reported = time.monotonic()
        time.sleep(0.2)


def gpu_state():
    """free, shared (games running) or exclusive (a timing job), from a non-blocking probe."""
    fd = lock_file('gpu')
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return 'free'
        except BlockingIOError:
            pass
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            return 'shared'
        except BlockingIOError:
            return 'exclusive'
    finally:
        os.close(fd)


def take_gpu(mode, label):
    """Holds the GPU lock in MODE for the rest of the job (the descriptor is inherited)."""
    held = os.environ.get('BB_HEAVY_GPU')
    if held == 'exclusive' or (held == 'shared' and mode == 'shared'):
        return
    if held == 'shared':
        fail(f'{label}: --gpu exclusive inside a shared GPU hold would wait for itself')
    # Requested, then (event gpu) granted: a job that ends while waiting leaves the request alone.
    record({'event': 'gpu-request', 'label': label, 'mode': mode, 'pid': os.getpid()})
    gate = lock_file('gpu-gate')
    if mode == 'exclusive':
        gate_wait = wait_flock(gate, fcntl.LOCK_EX, label, 'the GPU gate')
    else:
        gate_wait = wait_flock(gate, fcntl.LOCK_SH, label, 'a waiting GPU-exclusive job')
    fd = lock_file('gpu')
    waited = gate_wait + wait_flock(fd, fcntl.LOCK_EX if mode == 'exclusive' else fcntl.LOCK_SH, label,
                                    'the GPU (exclusive)' if mode == 'exclusive' else 'the GPU')
    os.close(gate)
    os.set_inheritable(fd, True)
    os.environ['BB_HEAVY_GPU'] = mode
    record({'event': 'gpu', 'label': label, 'mode': mode, 'waited_s': round(waited, 1), 'pid': os.getpid()})
    if waited >= 1:
        print(f'heavy: {label}: got the GPU ({mode}) after {waited:.0f} s', file=sys.stderr, flush=True)


def record(event):
    """One line in BB_HEAVY_DIR/history.jsonl (append; one write per line). Losing a history line
    must never stop a job, but a failure is reported."""
    event = {'time': datetime.datetime.now().isoformat(timespec='seconds'), **event}
    try:
        fd = os.open(os.path.join(lock_dir(), 'history.jsonl'), os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
        try:
            os.write(fd, (json.dumps(event) + '\n').encode())
        finally:
            os.close(fd)
    except OSError as error:
        print(f'heavy: history.jsonl not written: {error}', file=sys.stderr)


def busy_slots():
    """Slot names currently held (a non-blocking probe of each lock)."""
    held = []
    for name in ['build-priority', 'build-idle'] + slot_names('build') + slot_names('quick') + slot_names('sweep') + slot_names('playtest'):
        fd = try_lock(name)
        if fd is None:
            held.append(name)
        else:
            os.close(fd)
    return held


def describe(info):
    text = f"{info.get('label', '?')} (pid {info.get('pid', '?')}, since {info.get('since', '?')})"
    # Called for held slots only: with heavy.sh gone, a process of its job still holds the lock (its
    # reaper normally ends the job's group with heavy.sh; this is what remains if the reaper died too).
    pid, command_pid = info.get('pid'), info.get('command_pid')
    if pid and not alive(pid):
        group = f' (its process group {command_pid})' if command_pid else ''
        text += f' ORPHANED: heavy.sh pid {pid} is gone, a process of its job{group} still holds the slot'
    return text


def status():
    print(f'gpu: {gpu_state()}')
    for name in ['build-priority', 'build-idle'] + slot_names('build') + slot_names('quick') + slot_names('sweep') + slot_names('playtest'):
        fd = try_lock(name)
        if fd is not None:
            os.close(fd)
            print(f'{name}: free')
        else:
            print(f'{name}: held by {describe(holder(name))}')


def user_idle_seconds():
    """Seconds since the user last touched keyboard or mouse (IOHIDSystem HIDIdleTime), or None
    when it cannot be read. BB_HEAVY_IDLE_SECONDS overrides it (tests)."""
    override = os.environ.get('BB_HEAVY_IDLE_SECONDS')
    if override is not None:
        if not override.isdigit():
            fail('BB_HEAVY_IDLE_SECONDS must be a whole number of seconds')
        return int(override)
    try:
        out = subprocess.run(['ioreg', '-c', 'IOHIDSystem', '-d', '4'], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r'"HIDIdleTime" = (\d+)', out)
    return int(match.group(1)) // 1_000_000_000 if match else None


def user_state():
    """(idle, description). The user counts as idle after BB_HEAVY_IDLE_MINUTES (10) without
    input; an unreadable idle time counts as active, and says so."""
    minutes = os.environ.get('BB_HEAVY_IDLE_MINUTES', '10')
    if not minutes.isdigit():
        fail('BB_HEAVY_IDLE_MINUTES must be a whole number of minutes')
    idle = user_idle_seconds()
    if idle is None:
        return False, 'user idle time unreadable: treated as active'
    if idle >= int(minutes) * 60:
        return True, f'user idle {idle // 60} min'
    return False, f'user active (idle {idle // 60} min)'


def polite(pool, priority, gpu, command, idle=(False, '')):
    """The user shares this Mac and their apps were being starved: heavy jobs yield. Builds, tests
    and sweeps (build and quick pools) run as background QoS (taskpolicy -b: low CPU priority,
    efficiency cores, throttled I/O; slower, by the user's choice); correctness games (playtest
    pool) at nice 5 with a utility QoS clamp. Priority-lane jobs (P0 previews, shared builds) run
    at nice 5 with a utility clamp too: background QoS made a preview take 37 minutes. While the
    user is idle (user_state), new build, quick and sweep jobs also start at nice 5 with a utility
    clamp (performance cores allowed); running jobs keep what they started with. All keep the
    parallelism cap. Exclusive-GPU jobs (perf_run, timing) are exempt.
    Applied once, when the slot is taken, so nested calls inherit it. BB_HEAVY_POLITE=0 turns it
    off."""
    if os.environ.get('BB_HEAVY_POLITE', '1') == '0' or gpu == 'exclusive':
        return command, 'normal'
    if pool == 'playtest':
        os.nice(5)
        return ['taskpolicy', '-c', 'utility', *command], 'nice 5, utility QoS'
    # Compile parallelism: at most BB_HEAVY_BUILD_JOBS (8) jobs, lowering (never raising) a hint
    # the job already has. Plain `ninja` ignores these; build.sh and CMake/make honour them.
    cap = os.environ.get('BB_HEAVY_BUILD_JOBS', '8')
    if not cap.isdigit() or int(cap) < 1:
        fail('BB_HEAVY_BUILD_JOBS must be a positive whole number')
    for name in ('BB_BUILD_JOBS', 'CMAKE_BUILD_PARALLEL_LEVEL'):
        current = os.environ.get(name, '')
        os.environ[name] = str(min(int(current), int(cap))) if current.isdigit() else cap
    os.environ['MAKEFLAGS'] = f'-j{os.environ["BB_BUILD_JOBS"]}'
    jobs = os.environ['BB_BUILD_JOBS']
    if priority:
        os.nice(5)
        return ['taskpolicy', '-c', 'utility', *command], f'nice 5, utility QoS, {jobs} jobs'
    if idle[0]:
        os.nice(5)
        return ['taskpolicy', '-c', 'utility', *command], f'{idle[1]}: nice 5, utility QoS, {jobs} jobs'
    return ['taskpolicy', '-b', *command], f'{idle[1]}: background QoS, {jobs} jobs'


QUIET_POOLS = ('build', 'quick', 'sweep')


def quiet_mode():
    """While a --cpu-quiet job holds BB_HEAVY_DIR/cpu-quiet: 'stop' (build/quick/sweep jobs are
    paused and none starts) or 'ecore' (they run at background QoS, E-cores only); else None. A
    holder that wrote no mode (an older heavy.py) is 'stop'."""
    if not quiet_active():
        return None
    try:
        with open(os.path.join(lock_dir(), 'cpu-quiet.mode')) as f:
            return 'ecore' if f.read().strip() == 'ecore' else 'stop'
    except FileNotFoundError:
        return 'stop'


def quiet_active():
    """True while a --cpu-quiet job (a perf run) holds BB_HEAVY_DIR/cpu-quiet."""
    fd = lock_file('cpu-quiet')
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


def descendants(pid):
    """Every process below pid (pgrep -P, breadth first)."""
    found, queue = [], [pid]
    while queue:
        out = subprocess.run(['pgrep', '-P', str(queue.pop(0))], capture_output=True, text=True).stdout
        for child in (int(x) for x in out.split()):
            if child not in found:
                found.append(child)
                queue.append(child)
    return found


def job_processes(root, info):
    """A job's processes below its slot holder (root): its process tree, and every member of the
    command's own process group (heavy starts each command in a new session), which also holds
    those whose parent exited (re-parented to launchd, out of the tree), and still after its
    leader exited. A leader pid that now names another process (another start time), or a job
    from an older heavy.sh (no group of its own: its group is the caller's), gives the tree only."""
    pids = descendants(root)
    leader, recorded = info.get('command_pid'), info.get('command_start')
    if not leader:
        return pids
    out = subprocess.run(['ps', '-Ao', 'pid=,pgid='], capture_output=True, text=True).stdout
    table = [tuple(map(int, line.split())) for line in out.splitlines() if len(line.split()) == 2]
    try:
        current = process_start(leader)
    except OSError as error:
        print(f'heavy: job {root}: group leader {leader} unreadable ({error}): its tree only', file=sys.stderr, flush=True)
        return pids
    if current is not None:
        if recorded and tuple(recorded) != current:
            return pids  # the leader's pid now names another process: not this job's group
        if (leader, leader) not in table:
            return pids  # an older heavy.sh: the command did not lead a group of its own
    elif not recorded:
        return pids  # leader gone, and no record that it ever led a group
    # The job's group; also when its leader exited (a pid is not reused while its group lives).
    return pids + [pid for pid, group in table if group == leader and pid not in pids and pid != root]


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


_libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)


def process_start(pid):
    """The process's start time (seconds, microseconds) from the kernel (sysctl KERN_PROC_PID:
    kinfo_proc begins with it), or None when no such process exists. With the pid it names one
    process: a pid reused by a later process has another start, so CPU quiet never resumes or
    restores a stranger that inherited a pid it remembered. OSError when the kernel refuses."""
    mib = (ctypes.c_int * 4)(1, 14, 1, pid)  # CTL_KERN, KERN_PROC, KERN_PROC_PID
    buffer = ctypes.create_string_buffer(1024)
    size = ctypes.c_size_t(len(buffer))
    if _libc.sysctl(mib, 4, buffer, ctypes.byref(size), None, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, f'sysctl KERN_PROC_PID {pid}: {os.strerror(error)}')
    if size.value == 0:
        return None
    return struct.unpack_from('qi', buffer.raw, 0)


def start_or_none(pid, problems):
    """process_start, with a kernel refusal noted in PROBLEMS (and None) instead of raised: CPU quiet
    goes on with every other process and says so at the end."""
    try:
        return process_start(pid)
    except OSError as error:
        problems.append(f'{pid}: {error.strerror or error}')
        return None


def report_problems(label, what, problems):
    if problems:
        print(f'heavy: {label}: CPU quiet: {len(problems)} process(es) not {what} (identity unreadable): '
              f'{"; ".join(problems[:5])}', file=sys.stderr, flush=True)
        record({'event': 'quiet-problem', 'by': label, 'what': what, 'problems': problems[:20]})


def process_states(pids):
    """{pid: ps state letters} for the pids still alive."""
    if not pids:
        return {}
    out = subprocess.run(['ps', '-o', 'pid=,stat=', '-p', ','.join(map(str, pids))],
                         capture_output=True, text=True).stdout
    return {int(fields[0]): fields[1] for fields in (line.split() for line in out.splitlines()) if len(fields) == 2}


def pause_file(pid, kind='paused'):
    """BB_HEAVY_DIR/<kind>-<pid>.jsonl: when the job whose slot holder is pid was paused (kind
    paused) or demoted to E-core QoS (kind ecore) for a perf run, and when that ended (one line
    each); read by the job itself at its end."""
    return os.path.join(lock_dir(), f'{kind}-{pid}.jsonl')


def note_pause(pid, kind='paused', **entry):
    try:
        with open(pause_file(pid, kind), 'a') as f:
            f.write(json.dumps({'time': time.time(), **entry}) + '\n')
    except OSError as error:
        print(f'heavy: pause of job {pid} not noted: {error}', file=sys.stderr, flush=True)


def pauses(pid, kind='paused'):
    """(seconds paused or demoted, [perf runs that did it]) for the job with slot holder pid, from
    its kind's file (removed); one not yet ended counts until now."""
    try:
        with open(pause_file(pid, kind)) as f:
            entries = [json.loads(line) for line in f if line.strip()]
        os.unlink(pause_file(pid, kind))
    except FileNotFoundError:
        return 0.0, []
    except (OSError, ValueError) as error:
        print(f'heavy: pause file of job {pid} unreadable: {error}', file=sys.stderr, flush=True)
        return 0.0, ['unknown (pause file unreadable)']
    total, since, by = 0.0, None, []
    for entry in entries:
        if 'start' in entry:
            since = entry['time']
            if entry['start'] not in by:
                by.append(entry['start'])
        elif since is not None:
            total, since = total + entry['time'] - since, None
    if since is not None:
        total += time.time() - since
    return total, by


def pause_quiet_pools(label):
    """SIGSTOPs every running build/quick/sweep job (its whole process tree; for a quick job the
    command under its supervisor, so its time limit can pause) and notes the pause in the job's
    pause file. A process already stopped is left out: another pause (a test's own heavy.sh, a
    nested perf run) stopped it and resumes it, so this one must not. Returns [(job label, pids it
    stopped, slot holder pid)]."""
    paused, problems = [], []
    names = ['build-priority', 'build-idle'] + [n for pool in QUIET_POOLS for n in slot_names(pool)]
    for name in names:
        fd = try_lock(name)
        if fd is not None:
            os.close(fd)
            continue
        info = holder(name)
        if not info.get('pid') or not alive(info['pid']):
            continue
        root = info['pid']
        tree = [] if info.get('pool') == 'quick' else [root]
        handled, pids, foreign = set(), [], []
        for _ in range(4):  # stop the tree; again for processes started while stopping it
            tree += [p for p in job_processes(root, info) if p not in tree]
            new = [p for p in tree if p not in handled]
            if not new:
                break
            states = process_states(new)
            for pid in new:
                handled.add(pid)
                if states.get(pid, '').startswith('T'):
                    foreign.append(pid)
                    continue
                start = start_or_none(pid, problems)
                if start is None:
                    continue
                try:
                    os.kill(pid, signal.SIGSTOP)
                    pids.append((pid, start))
                except ProcessLookupError:
                    pass
        note_pause(root, start=label)
        paused.append((info.get('label', name), pids, root))
        left = f'; {len(foreign)} already stopped by another pause, left to it' if foreign else ''
        print(f'heavy: {label}: CPU quiet: paused {info.get("label", name)} ({name}, {len(pids)} processes{left})',
              file=sys.stderr, flush=True)
        record({'event': 'quiet-pause', 'by': label, 'label': info.get('label'), 'slot': name, 'pids': len(pids),
                **({'already_stopped': len(foreign)} if foreign else {})})
    if not paused:
        print(f'heavy: {label}: CPU quiet: no build, quick or sweep job was running', file=sys.stderr, flush=True)
    report_problems(label, 'paused', problems)
    return paused


PRIO_DARWIN_PROCESS, PRIO_DARWIN_BG = 4, 0x1000  # <sys/resource.h>
BACKGROUND_PRIORITY = 4  # ps pri of a darwin-background process (taskpolicy -b)


def process_table(pids):
    """{pid: (parent pid, ps scheduling priority)} for the pids still alive."""
    if not pids:
        return {}
    out = subprocess.run(['ps', '-o', 'pid=,ppid=,pri=', '-p', ','.join(map(str, pids))],
                         capture_output=True, text=True).stdout
    table = {}
    for fields in (line.split() for line in out.splitlines()):
        if len(fields) == 3 and all(x.lstrip('-').isdigit() for x in fields):
            table[int(fields[0])] = (int(fields[1]), int(fields[2]))
    return table


def quiet_jobs():
    """[(slot, job label, slot holder pid, slot info)] of the running build/quick/sweep jobs."""
    jobs = []
    for name in ['build-priority', 'build-idle'] + [n for pool in QUIET_POOLS for n in slot_names(pool)]:
        fd = try_lock(name)
        if fd is not None:
            os.close(fd)
            continue
        info = holder(name)
        if info.get('pid') and alive(info['pid']):
            jobs.append((name, info.get('label', name), info['pid'], info))
    return jobs


def demote_quiet_pools(label, demoted, seen, noted):
    """--cpu-quiet=ecore, every second: each process of a running build/quick/sweep job (its whole
    tree) not yet seen goes to darwin background QoS (efficiency cores, low CPU and I/O priority),
    setpriority(PRIO_DARWIN_PROCESS, PRIO_DARWIN_BG), as taskpolicy -b does. Children forked
    later inherit it. demoted: pid -> slot holder, the processes this run demoted or that inherited
    it from them (it restores those). A process already in the background (a job started with
    taskpolicy -b) is left alone and not restored. Processes are kept as (pid, start time), so a
    reused pid is a new process, never one remembered (process_start)."""
    for name, job, root, info in quiet_jobs():
        tree = [root] + job_processes(root, info)
        problems = []
        starts = {pid: start_or_none(pid, problems) for pid in tree}
        report_problems(label, 'demoted', problems)
        table = process_table([p for p in tree if starts[p] and (p, starts[p]) not in seen])
        count = 0
        for pid, (ppid, priority) in table.items():
            seen.add((pid, starts[pid]))
            if priority <= BACKGROUND_PRIORITY:
                parent = demoted.get(ppid)
                if parent and start_or_none(ppid, []) == parent[1]:
                    demoted[pid] = (root, starts[pid])  # inherited from a process this run demoted
                continue
            if start_or_none(pid, []) != starts[pid]:
                continue  # died between the reads (its pid may already name another process)
            try:
                os.setpriority(PRIO_DARWIN_PROCESS, pid, PRIO_DARWIN_BG)
                demoted[pid] = (root, starts[pid])
                count += 1
            except (ProcessLookupError, PermissionError):
                pass
        if root not in noted:
            noted[root] = job
            note_pause(root, 'ecore', start=label)
            print(f'heavy: {label}: CPU quiet (E-cores): {job} ({name}) at background QoS, {count} processes',
                  file=sys.stderr, flush=True)
            record({'event': 'quiet-ecore', 'by': label, 'label': job, 'slot': name, 'pids': count})


def restore_demoted(label, demoted, noted):
    """The end of --cpu-quiet=ecore: the jobs' records first, then each demoted process back."""
    for root, job in noted.items():
        note_pause(root, 'ecore', end=label)
    problems = []
    for pid, (_, start) in demoted.items():
        if start_or_none(pid, problems) != start:
            continue  # gone, the pid now names another process, or unreadable (reported)
        try:
            os.setpriority(PRIO_DARWIN_PROCESS, pid, 0)
        except (ProcessLookupError, PermissionError):
            pass
    report_problems(label, 'restored (left at background QoS)', problems)
    for job in noted.values():
        print(f'heavy: {label}: CPU quiet over: {job} back from E-cores', file=sys.stderr, flush=True)


def top_processes(count=5):
    """The COUNT processes using most CPU now (ps -r): 'pcpu% pid pgid <group> <command> cwd <dir>'
    each, so a process running outside any heavy job names its owner (its group, its directory)."""
    out = subprocess.run(['ps', '-Ao', 'pcpu=,pid=,pgid=,comm=', '-r'], capture_output=True, text=True).stdout
    top = []
    for line in out.splitlines()[:count]:
        fields = line.split(None, 3)
        if len(fields) != 4:
            continue
        pcpu, pid, pgid, command = fields
        cwd = subprocess.run(['lsof', '-a', '-p', pid, '-d', 'cwd', '-Fn'], capture_output=True,
                             text=True).stdout
        cwd = next((l[1:] for l in cwd.splitlines() if l.startswith('n')), '?')
        top.append(f'{pcpu}% {pid} pgid {pgid} {os.path.basename(command)} cwd {cwd}')
    return top


def settle(label, limit, bound):
    """--settle LOAD,SECONDS: before the perf run starts (other jobs already paused or demoted),
    waits until the 1-minute load average is below LOAD. Past SECONDS it refuses the run (exit 75),
    naming the load and the top processes."""
    started = time.monotonic()
    first = os.getloadavg()[0]
    while os.getloadavg()[0] >= limit:
        if time.monotonic() - started >= bound:
            top = top_processes()
            record({'event': 'settle', 'label': label, 'limit': limit, 'load': round(os.getloadavg()[0], 2),
                    'waited_s': round(time.monotonic() - started, 1), 'settled': False, 'top': top})
            fail(f'{label}: SETTLE FAILED: 1-minute load {os.getloadavg()[0]:.1f} still at or above {limit} after '
                 f'{bound} s; top: {"; ".join(top)}', 75)
        time.sleep(2)
    waited = time.monotonic() - started
    record({'event': 'settle', 'label': label, 'limit': limit, 'load_before': round(first, 2),
            'load': round(os.getloadavg()[0], 2), 'waited_s': round(waited, 1), 'settled': True})
    print(f'heavy: {label}: settled: 1-minute load {os.getloadavg()[0]:.1f} below {limit} after {waited:.0f} s '
          f'(was {first:.1f})', file=sys.stderr, flush=True)


def resume_paused(label, paused):
    """The end of a stop-mode quiet: each job's pause record first (a job that ends at once reads
    a closed pause), then SIGCONT to each process it stopped that is still the same process."""
    problems = []
    for job, pids, root in paused:
        note_pause(root, end=label)
        for pid, start in reversed(pids):
            if start_or_none(pid, problems) != start:
                continue  # gone, the pid now names another process, or unreadable (reported)
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        print(f'heavy: {label}: CPU quiet over: resumed {job}', file=sys.stderr, flush=True)
    report_problems(label, 'resumed (left stopped)', problems)


def start_quiet(label, mode='stop'):
    """--cpu-quiet: until this job ends (at most BB_HEAVY_QUIET_MAX seconds), no build, quick or
    sweep job is admitted and the running ones are paused. A forked watcher holds
    BB_HEAVY_DIR/cpu-quiet, pauses the jobs, waits for this process (which goes on to exec the
    perf run) to end or the bound to pass, then resumes them and says so.
    --cpu-quiet=ecore: jobs are not paused but demoted to background QoS (E-cores), new ones
    start there, and the demoted processes are restored at the end (demote_quiet_pools)."""
    bound = seconds('BB_HEAVY_QUIET_MAX', 1200)
    lock = lock_file('cpu-quiet')
    fcntl.flock(lock, fcntl.LOCK_EX)
    with open(os.path.join(lock_dir(), 'cpu-quiet.mode'), 'w') as f:
        f.write(mode + '\n')
    top = top_processes()
    record({'event': 'quiet-start', 'by': label, 'mode': mode, 'load': [round(x, 2) for x in os.getloadavg()],
            'top': top})
    print(f'heavy: {label}: CPU quiet ({mode}) starts; top CPU: {"; ".join(top)}', file=sys.stderr, flush=True)
    target = os.getpid()
    if os.fork():
        os.close(lock)  # the watcher keeps the lock
        return
    os.setsid()
    started = time.monotonic()
    if mode == 'ecore':
        demoted, seen, noted = {}, set(), {}
        while alive(target) and time.monotonic() - started < bound:
            demote_quiet_pools(label, demoted, seen, noted)
            time.sleep(1)
        if not noted:
            print(f'heavy: {label}: CPU quiet (E-cores): no build, quick or sweep job was running',
                  file=sys.stderr, flush=True)
        if alive(target):
            print(f'heavy: {label}: CPU QUIET BOUND REACHED after {bound} s: restoring demoted jobs while '
                  f'this job still runs', file=sys.stderr, flush=True)
        restore_demoted(label, demoted, noted)
        record({'event': 'quiet-end', 'by': label, 'mode': 'ecore', 'seconds': round(time.monotonic() - started, 1),
                'bound_reached': alive(target), 'restored': list(noted.values()), 'processes': len(demoted)})
        os._exit(0)
    paused = pause_quiet_pools(label)
    while alive(target) and time.monotonic() - started < bound:
        time.sleep(1)
    if alive(target):
        print(f'heavy: {label}: CPU QUIET BOUND REACHED after {bound} s: resuming paused jobs while '
              f'this job still runs', file=sys.stderr, flush=True)
    resume_paused(label, paused)
    record({'event': 'quiet-end', 'by': label, 'seconds': round(time.monotonic() - started, 1),
            'bound_reached': alive(target), 'resumed': [job for job, _, _ in paused]})
    os._exit(0)


def announce(label):
    """With BB_HEAVY_ANNOUNCE set (play_route.py sets it), says on stderr that the command starts
    now: everything before was waiting for a slot or the GPU, not the command's own startup."""
    if os.environ.get('BB_HEAVY_ANNOUNCE'):
        print(f'heavy: {label}: running', file=sys.stderr, flush=True)


def run(pool, priority, gpu, label, command, pool_given=False, quiet=False, settle_at=None):
    held = os.environ.get('BB_HEAVY_HELD')
    if held and quiet:
        fail(f'{label}: --cpu-quiet inside {held} would pause the job it runs in')
    if not held and pool in TREE_POOLS:
        take_worktree(label, pool)
    if held:
        held_pool = held.split(':', 1)[0].rstrip('0123456789').replace('build-priority', 'build')
        if pool_given and pool != held_pool and pool != 'playtest':
            print(f'heavy: {label}: started inside {held}; it runs in that {held_pool} slot, not a '
                  f'{pool} slot (start it as its own heavy.sh call to free the slot)',
                  file=sys.stderr, flush=True)
        if gpu:
            take_gpu(gpu, label)
        announce(label)
        os.execvp(command[0], command)
    started, reported = time.monotonic(), time.monotonic()
    idle, idle_checked = (False, ''), None
    while True:
        # While the user is idle the build pool admits one more job (build-idle); a job already
        # in it keeps running when the user returns. Checked at most every 10 s while waiting.
        if pool in QUIET_POOLS and (idle_checked is None or time.monotonic() - idle_checked >= 10):
            idle, idle_checked = user_state(), time.monotonic()
        ecore = pool in QUIET_POOLS and quiet_mode() == 'ecore'
        if pool in QUIET_POOLS and not ecore and quiet_active():
            # A perf run wants the CPU quiet: nothing new starts until it ends.
            if time.monotonic() - reported >= WAIT_REPORT_SECONDS:
                print(f'heavy: {label}: waiting: a perf run keeps the CPU quiet', file=sys.stderr, flush=True)
                reported = time.monotonic()
            time.sleep(1)
            continue
        names = ((['build-priority'] if priority else []) + slot_names(pool) +
                 (['build-idle'] if pool == 'build' and idle[0] else []))
        for name in names:
            fd = try_lock(name)
            if fd is None:
                continue
            info = {'label': label, 'pid': os.getpid(), 'pool': pool, 'priority': priority,
                    'since': datetime.datetime.now().isoformat(timespec='seconds'),
                    'command': command}
            os.ftruncate(fd, 0)
            os.pwrite(fd, json.dumps(info).encode(), 0)
            os.fsync(fd)
            for kind in ('paused', 'ecore'):  # left by an earlier job with this pid (killed before its end)
                try:
                    os.unlink(pause_file(os.getpid(), kind))
                except FileNotFoundError:
                    pass
            waited = time.monotonic() - started
            command, policy = polite(pool, priority, gpu, command, idle)
            if ecore:
                # Admitted during a perf run's E-core quiet: background QoS for its whole life.
                command = ['taskpolicy', '-b', *command]
                policy = f'{policy}; background QoS (E-cores) for a perf run\'s CPU quiet'
                record({'event': 'quiet-ecore-admit', 'label': label, 'pool': pool})
            if pool in ('build', 'quick', 'sweep') and not priority and policy != 'normal':
                print(f'heavy: {label}: {policy}', file=sys.stderr, flush=True)
            in_place = os.environ.pop('BB_HEAVY_EXEC', '') == '1'
            record({'event': 'start', 'label': label, 'pool': pool, 'slot': name, 'priority': priority,
                    'waited_s': round(waited, 1), 'load': [round(x, 2) for x in os.getloadavg()],
                    'held': busy_slots(), 'pid': os.getpid(), 'policy': policy,
                    'command': ' '.join(command)[:400], 'cwd': os.getcwd(),
                    **({'worktree': TREE['root'], 'head': TREE['head'], 'dirty': TREE['dirty']} if TREE else {}),
                    **({'end': 'recorded by the caller (exec in place)'} if in_place else {})})
            if waited >= 1:
                print(f'heavy: {label}: got {name} after {waited:.0f} s', file=sys.stderr, flush=True)
            os.set_inheritable(fd, True)
            os.environ['BB_HEAVY_HELD'] = f'{name}:{label}'
            if gpu:
                take_gpu(gpu, label)
            if quiet:
                start_quiet(label, quiet)
                if settle_at:
                    settle(label, *settle_at)
            announce(label)
            if pool == 'quick':
                supervise_quick(label, command, fd, info)
            if not in_place:
                supervise(label, pool, command, fd, info)
            try:
                os.execvp(command[0], command)
            except OSError as error:
                fail(f'cannot run {command[0]}: {error}', 127)
        if time.monotonic() - reported >= WAIT_REPORT_SECONDS:
            held = '; '.join(f'{name}: {describe(holder(name))}' for name in names)
            print(f'heavy: {label}: waiting for a slot ({held})', file=sys.stderr, flush=True)
            reported = time.monotonic()
        time.sleep(1)


TREE = {}  # the job's worktree at start: root, HEAD and a digest of its tracked changes
TREE_POOLS = ('build', 'quick', 'sweep')  # jobs that compile or run from the checkout


def git(root, *args):
    result = subprocess.run(['git', '-C', root, *args], capture_output=True, text=True, timeout=120)
    return result.stdout if result.returncode == 0 else None


def tree_state(path):
    """(root, HEAD, digest of the tracked changes, dirty) of the git worktree containing path, or
    None outside one. Submodule content changes (build.sh patches FSR once) and untracked files
    (logs, out/) do not count."""
    top = git(path, 'rev-parse', '--show-toplevel')
    if top is None:
        return None
    root = top.strip()
    head = (git(root, 'rev-parse', 'HEAD') or '').strip()
    diff = git(root, 'diff', 'HEAD', '--ignore-submodules=dirty')
    if diff is None:
        fail(f'cannot read the state of worktree {root}')
    return root, head, hashlib.sha256(diff.encode()).hexdigest(), bool(diff)


def take_worktree(label, pool):
    """One build/quick/sweep job per worktree: a second one is refused at once, naming the holder.
    Records the worktree's HEAD and tracked changes for the end check. BB_HEAVY_SHARED_TREE=1
    lets a read-only job share the tree."""
    state = tree_state(os.getcwd())
    if state is None:
        return
    root, head, digest, dirty = state
    TREE.update(root=root, head=head, digest=digest, dirty=dirty)
    if os.environ.get('BB_HEAVY_SHARED_TREE') == '1':
        return
    name = 'tree-' + hashlib.sha256(root.encode()).hexdigest()[:16]
    fd = try_lock(name)
    if fd is None:
        fail(f'{label}: refused: worktree {root} is busy with {describe(holder(name))}; one build/test job '
             f'per worktree (BB_HEAVY_SHARED_TREE=1 for a read-only job)')
    os.ftruncate(fd, 0)
    os.pwrite(fd, json.dumps({'label': label, 'pid': os.getpid(), 'pool': pool, 'worktree': root,
                              'since': datetime.datetime.now().isoformat(timespec='seconds')}).encode(), 0)
    os.set_inheritable(fd, True)


def tree_changed(label):
    """At the end: why the job's result is invalid (HEAD moved, or tracked files edited during the
    run), or None."""
    if not TREE:
        return None
    state = tree_state(TREE['root'])
    if state is None:
        return f'worktree {TREE["root"]} is gone'
    _, head, digest, _ = state
    if head != TREE['head']:
        return f'HEAD of {TREE["root"]} changed during the run ({TREE["head"][:10]} -> {head[:10]})'
    if digest != TREE['digest']:
        return f'tracked files in {TREE["root"]} were edited during the run'
    return None


def seconds(variable, default):
    value = os.environ.get(variable, str(default))
    if not value.isdigit() or int(value) < 1:
        fail(f'{variable} must be a positive whole number of seconds')
    return int(value)


def record_end(label, pool, status, started, **extra):
    """The job's end event: exit status, or the signal that killed it, and its duration. A job whose
    worktree changed during the run is INVALID: said on stderr and in the event, and a success
    becomes exit 125 so no caller takes it for a pass. A job paused for perf runs says so on
    stderr and in the event (paused_s, paused_by): a wall-clock timeout inside it (alarm(), a
    deadline) may have fired on resume, so a failure is not yet a result."""
    event = {'event': 'end', 'label': label, 'pool': pool,
             'seconds': round(time.monotonic() - started, 1), 'load': [round(x, 2) for x in os.getloadavg()]}
    if status < 0:
        event.update(status=128 - status, signal=signal.Signals(-status).name)
    else:
        event['status'] = status
    paused, by = pauses(os.getpid())
    if by:
        code = event.get('signal') or f'exit {event["status"]}'
        print(f'heavy: {label}: PAUSED {paused:.0f} s for perf runs ({"; ".join(by)}); ended with {code}'
              + ('' if event['status'] == 0 else
                 ': a wall-clock timeout in the job may have fired on resume; rerun before calling it a failure'),
              file=sys.stderr, flush=True)
        event.update(paused_s=round(paused, 1), paused_by=by)
    slowed, slowed_by = pauses(os.getpid(), 'ecore')
    if slowed_by:
        code = event.get('signal') or f'exit {event["status"]}'
        print(f'heavy: {label}: ran {slowed:.0f} s at E-core QoS for perf runs ({"; ".join(slowed_by)}); ended with {code}'
              + ('' if event['status'] == 0 else
                 ': it ran slower, so a tight time limit in the job may have fired; rerun before calling it a failure'),
              file=sys.stderr, flush=True)
        event.update(ecore_s=round(slowed, 1), ecore_by=slowed_by)
    invalid = tree_changed(label)
    if invalid:
        print(f'heavy: {label}: INVALID: {invalid}; the result does not belong to one tree',
              file=sys.stderr, flush=True)
        event['invalid'] = invalid
        if event['status'] == 0:
            event['status'] = 125
    record({**event, **extra})
    return event['status']


def spawn_in_session(command):
    """Starts COMMAND in a new session (its own process group, so CPU quiet acts on the whole job
    and on none of the caller) under a reaper, heavy.py --reap-for PID: the group's leader, which
    waits for the command and SIGKILLs the whole group if this heavy.py dies for any reason (a
    SIGKILL to the caller's group, a stopped task), so a job never outlives the slot it holds.
    Returns the reaper's Popen; its exit is the command's."""
    return subprocess.Popen([sys.executable, os.path.abspath(__file__), '--reap-for', str(os.getpid()), '--', *command],
                            start_new_session=True, close_fds=False)


def reap(watched, command):
    """--reap-for PID -- COMMAND: runs COMMAND in this process group, then ignores SIGINT/SIGTERM/
    SIGHUP (heavy.py sends them to the whole group, so the command gets them; this process stays
    to report). Polls its parent: once it is no longer PID, the group is SIGKILLed, this process
    included. Exits with the command's status, or by the command's signal. A signal to the group
    between the spawn and the SIG_IGN ends this process only; the command gets it too, and the
    inherited slot descriptor keeps the slot held until the command exits."""
    try:
        child = subprocess.Popen(command, close_fds=False)
    except OSError as error:
        print(f'heavy: cannot run {command[0]}: {error}', file=sys.stderr, flush=True)
        os._exit(127)
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):  # after the spawn: not inherited
        signal.signal(signum, signal.SIG_IGN)
    while True:
        done, status = os.waitpid(child.pid, os.WNOHANG)
        if done:
            break
        if os.getppid() != watched:
            os.killpg(0, signal.SIGKILL)
        time.sleep(0.1)
    if os.WIFSIGNALED(status):
        signal.signal(os.WTERMSIG(status), signal.SIG_DFL)
        os.kill(os.getpid(), os.WTERMSIG(status))
    os._exit(os.WEXITSTATUS(status))


def session_info(info, child):
    """The slot record with the job's process group: its leader (the reaper) and its start."""
    try:
        start = process_start(child.pid)
    except OSError:
        start = None
    return {**info, 'command_pid': child.pid, 'command_start': list(start) if start else None}


def supervise(label, pool, command, fd=None, info=None):
    """Runs the job's command as a child in its own session (its own process group, so CPU quiet
    finds every process of the job, including ones whose parent exited, and none of the caller's),
    forwards SIGINT/SIGTERM/SIGHUP to that group, records its end (exit status, or the signal that
    killed it, and the duration) and exits with its status. A caller that needs the job's pid to
    be the command's (play_route.py: the game) sets BB_HEAVY_EXEC=1 and records the end itself."""
    started = time.monotonic()
    try:
        child = spawn_in_session(command)
    except OSError as error:
        record_end(label, pool, 127, started, error=str(error))
        fail(f'cannot run {command[0]}: {error}', 127)
    if fd is not None:  # the slot names the command too, so status can tell an orphan apart
        info = session_info(info, child)
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps(info).encode(), 0)
    def forward(signum, _):
        try:
            os.killpg(child.pid, signum)
        except ProcessLookupError:
            pass
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, forward)
    sys.exit(record_end(label, pool, child.wait(), started))


def supervise_quick(label, command, fd=None, info=None):
    """Runs a quick job as a child in its own process group, enforcing the time limit; exits."""
    warn, limit = seconds('BB_HEAVY_QUICK_WARN', 300), seconds('BB_HEAVY_QUICK_LIMIT', 600)
    for name in ('BB_BUILD_JOBS', 'CMAKE_BUILD_PARALLEL_LEVEL'):
        os.environ[name] = QUICK_JOBS
    os.environ['MAKEFLAGS'] = f'-j{QUICK_JOBS}'
    try:
        child = spawn_in_session(command)
    except OSError as error:
        fail(f'cannot run {command[0]}: {error}', 127)
    if fd is not None:  # the command's process group, for CPU quiet
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps(session_info(info, child)).encode(), 0)
    started, warned = time.monotonic(), False
    forward = lambda signum, _: os.killpg(child.pid, signum)
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, forward)
    def finish(status, **extra):
        sys.exit(record_end(label, 'quick', status, started, **extra))
    last = time.monotonic()
    while True:
        try:
            finish(child.wait(timeout=1))
        except subprocess.TimeoutExpired:
            pass
        now = time.monotonic()
        if quiet_mode() == 'stop':
            started += now - last  # paused for a perf run: the time limit pauses too
        last = now
        elapsed = now - started
        if not warned and elapsed >= warn:
            print(f'heavy: WARNING: quick job {label} still running after {warn} s '
                  f'(killed at {limit} s; long jobs belong in the build pool)', file=sys.stderr, flush=True)
            warned = True
        if elapsed >= limit:
            print(f'heavy: KILLED quick job {label} after {limit} s: {" ".join(command)}',
                  file=sys.stderr, flush=True)
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            finish(124, killed='time limit')


def main(argv):
    if argv == ['status']:
        return status()
    if len(argv) > 3 and argv[0] == '--reap-for' and argv[1].isdigit() and argv[2] == '--':
        reap(int(argv[1]), argv[3:])
    pool, priority, gpu, pool_given, quiet, settle_at = 'build', False, None, False, False, None
    while argv and argv[0].startswith('--') and argv[0] != '--':
        if argv[0] == '--priority':
            priority, argv = True, argv[1:]
        elif argv[0] == '--pool' and len(argv) > 1 and argv[1] in POOLS:
            pool, argv, pool_given = argv[1], argv[2:], True
        elif argv[0] in ('--cpu-quiet', '--cpu-quiet=stop'):
            quiet, argv = 'stop', argv[1:]
        elif argv[0] == '--cpu-quiet=ecore':
            quiet, argv = 'ecore', argv[1:]
        elif argv[0] == '--settle' and len(argv) > 1:
            load, _, wait = argv[1].partition(',')
            try:
                settle_at = (float(load), int(wait))
            except ValueError:
                settle_at = None
            if not settle_at or settle_at[0] <= 0 or settle_at[1] < 1:
                fail(f'--settle needs LOAD,SECONDS (a positive load and whole seconds), not {argv[1]}')
            argv = argv[2:]
        elif argv[0] == '--gpu' and len(argv) > 1 and argv[1] in ('shared', 'exclusive'):
            gpu, argv = argv[1], argv[2:]
        else:
            fail(f'unknown option {argv[0]} (--pool build|playtest|quick|sweep, --priority, --gpu shared|exclusive, '
                 f'--cpu-quiet[=stop|ecore], --settle LOAD,SECONDS)')
    if pool == 'playtest' and gpu is None:
        gpu = 'shared'  # a game uses the GPU
    if settle_at and not quiet:
        fail('--settle is for --cpu-quiet runs (the load can only fall while other jobs are quiet)')
    if quiet and gpu != 'exclusive':
        fail('--cpu-quiet is for --gpu exclusive jobs (perf runs)')
    if priority and pool != 'build':
        fail('--priority is for the build pool (shared builds); the playtest pool is dedicated already')
    if len(argv) < 3 or argv[1] != '--' or not argv[0]:
        fail('usage: heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive] LABEL -- '
             'COMMAND [ARG...] | heavy.sh status')
    run(pool, priority, gpu, argv[0], argv[2:], pool_given, quiet, settle_at)


if __name__ == '__main__':
    main(sys.argv[1:])
