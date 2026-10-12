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
import datetime
import fcntl
import hashlib
import json
import os
import re
import signal
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
    pid, command_pid = info.get('pid'), info.get('command_pid')
    if pid and command_pid and not alive(pid) and alive(command_pid):
        text += f' ORPHANED: heavy.sh pid {pid} is gone, its command pid {command_pid} still holds the slot'
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


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def process_states(pids):
    """{pid: ps state letters} for the pids still alive."""
    if not pids:
        return {}
    out = subprocess.run(['ps', '-o', 'pid=,stat=', '-p', ','.join(map(str, pids))],
                         capture_output=True, text=True).stdout
    return {int(fields[0]): fields[1] for fields in (line.split() for line in out.splitlines()) if len(fields) == 2}


def pause_file(pid):
    """BB_HEAVY_DIR/paused-<pid>.jsonl: when the job whose slot holder is pid was paused for a perf
    run and resumed (one line each), read by the job itself at its end."""
    return os.path.join(lock_dir(), f'paused-{pid}.jsonl')


def note_pause(pid, **entry):
    try:
        with open(pause_file(pid), 'a') as f:
            f.write(json.dumps({'time': time.time(), **entry}) + '\n')
    except OSError as error:
        print(f'heavy: pause of job {pid} not noted: {error}', file=sys.stderr, flush=True)


def pauses(pid):
    """(seconds paused, [perf runs that paused it]) for the job with slot holder pid, from its
    pause file (removed); a pause not yet resumed counts until now."""
    try:
        with open(pause_file(pid)) as f:
            entries = [json.loads(line) for line in f if line.strip()]
        os.unlink(pause_file(pid))
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
    paused = []
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
            tree += [p for p in descendants(root) if p not in tree]
            new = [p for p in tree if p not in handled]
            if not new:
                break
            states = process_states(new)
            for pid in new:
                handled.add(pid)
                if states.get(pid, '').startswith('T'):
                    foreign.append(pid)
                    continue
                try:
                    os.kill(pid, signal.SIGSTOP)
                    pids.append(pid)
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
    return paused


def start_quiet(label):
    """--cpu-quiet: until this job ends (at most BB_HEAVY_QUIET_MAX seconds), no build, quick or
    sweep job is admitted and the running ones are paused. A forked watcher holds
    BB_HEAVY_DIR/cpu-quiet, pauses the jobs, waits for this process (which goes on to exec the
    perf run) to end or the bound to pass, then resumes them and says so."""
    bound = seconds('BB_HEAVY_QUIET_MAX', 1200)
    lock = lock_file('cpu-quiet')
    fcntl.flock(lock, fcntl.LOCK_EX)
    target = os.getpid()
    if os.fork():
        os.close(lock)  # the watcher keeps the lock
        return
    os.setsid()
    started = time.monotonic()
    paused = pause_quiet_pools(label)
    while alive(target) and time.monotonic() - started < bound:
        time.sleep(1)
    if alive(target):
        print(f'heavy: {label}: CPU QUIET BOUND REACHED after {bound} s: resuming paused jobs while '
              f'this job still runs', file=sys.stderr, flush=True)
    for job, pids, root in paused:
        note_pause(root, end=label)  # before resuming: a job that ends at once reads a closed pause
        for pid in reversed(pids):
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        print(f'heavy: {label}: CPU quiet over: resumed {job}', file=sys.stderr, flush=True)
    record({'event': 'quiet-end', 'by': label, 'seconds': round(time.monotonic() - started, 1),
            'bound_reached': alive(target), 'resumed': [job for job, _, _ in paused]})
    os._exit(0)


def announce(label):
    """With BB_HEAVY_ANNOUNCE set (play_route.py sets it), says on stderr that the command starts
    now: everything before was waiting for a slot or the GPU, not the command's own startup."""
    if os.environ.get('BB_HEAVY_ANNOUNCE'):
        print(f'heavy: {label}: running', file=sys.stderr, flush=True)


def run(pool, priority, gpu, label, command, pool_given=False, quiet=False):
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
        if pool in QUIET_POOLS and quiet_active():
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
            try:  # a pause file left by an earlier job with this pid (killed before its end)
                os.unlink(pause_file(os.getpid()))
            except FileNotFoundError:
                pass
            waited = time.monotonic() - started
            command, policy = polite(pool, priority, gpu, command, idle)
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
                start_quiet(label)
            announce(label)
            if pool == 'quick':
                supervise_quick(label, command)
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
    invalid = tree_changed(label)
    if invalid:
        print(f'heavy: {label}: INVALID: {invalid}; the result does not belong to one tree',
              file=sys.stderr, flush=True)
        event['invalid'] = invalid
        if event['status'] == 0:
            event['status'] = 125
    record({**event, **extra})
    return event['status']


def supervise(label, pool, command, fd=None, info=None):
    """Runs the job's command as a child in the same process group, forwards SIGINT/SIGTERM/SIGHUP
    to it, records its end (exit status, or the signal that killed it, and the duration) and exits
    with its status. A caller that needs the job's pid to be the command's (play_route.py: the
    game) sets BB_HEAVY_EXEC=1 and records the end itself."""
    started = time.monotonic()
    try:
        child = subprocess.Popen(command, close_fds=False)
    except OSError as error:
        record_end(label, pool, 127, started, error=str(error))
        fail(f'cannot run {command[0]}: {error}', 127)
    if fd is not None:  # the slot names the command too, so status can tell an orphan apart
        info = {**info, 'command_pid': child.pid}
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps(info).encode(), 0)
    def forward(signum, _):
        try:
            child.send_signal(signum)
        except ProcessLookupError:
            pass
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, forward)
    sys.exit(record_end(label, pool, child.wait(), started))


def supervise_quick(label, command):
    """Runs a quick job as a child in its own process group, enforcing the time limit; exits."""
    warn, limit = seconds('BB_HEAVY_QUICK_WARN', 300), seconds('BB_HEAVY_QUICK_LIMIT', 600)
    for name in ('BB_BUILD_JOBS', 'CMAKE_BUILD_PARALLEL_LEVEL'):
        os.environ[name] = QUICK_JOBS
    os.environ['MAKEFLAGS'] = f'-j{QUICK_JOBS}'
    try:
        child = subprocess.Popen(command, start_new_session=True, close_fds=False)
    except OSError as error:
        fail(f'cannot run {command[0]}: {error}', 127)
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
        if quiet_active():
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
    pool, priority, gpu, pool_given, quiet = 'build', False, None, False, False
    while argv and argv[0].startswith('--') and argv[0] != '--':
        if argv[0] == '--priority':
            priority, argv = True, argv[1:]
        elif argv[0] == '--pool' and len(argv) > 1 and argv[1] in POOLS:
            pool, argv, pool_given = argv[1], argv[2:], True
        elif argv[0] == '--cpu-quiet':
            quiet, argv = True, argv[1:]
        elif argv[0] == '--gpu' and len(argv) > 1 and argv[1] in ('shared', 'exclusive'):
            gpu, argv = argv[1], argv[2:]
        else:
            fail(f'unknown option {argv[0]} (--pool build|playtest|quick|sweep, --priority, --gpu shared|exclusive, --cpu-quiet)')
    if pool == 'playtest' and gpu is None:
        gpu = 'shared'  # a game uses the GPU
    if quiet and gpu != 'exclusive':
        fail('--cpu-quiet is for --gpu exclusive jobs (perf runs)')
    if priority and pool != 'build':
        fail('--priority is for the build pool (shared builds); the playtest pool is dedicated already')
    if len(argv) < 3 or argv[1] != '--' or not argv[0]:
        fail('usage: heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive] LABEL -- '
             'COMMAND [ARG...] | heavy.sh status')
    run(pool, priority, gpu, argv[0], argv[2:], pool_given, quiet)


if __name__ == '__main__':
    main(sys.argv[1:])
