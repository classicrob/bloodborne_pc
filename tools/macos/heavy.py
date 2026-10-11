#!/usr/bin/env python3
"""Machine-wide limit on heavy jobs (GPU builds, test suites, game runs, benchmarks).

  tools/macos/heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive]
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
    return f"{info.get('label', '?')} (pid {info.get('pid', '?')}, since {info.get('since', '?')})"


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


def announce(label):
    """With BB_HEAVY_ANNOUNCE set (play_route.py sets it), says on stderr that the command starts
    now: everything before was waiting for a slot or the GPU, not the command's own startup."""
    if os.environ.get('BB_HEAVY_ANNOUNCE'):
        print(f'heavy: {label}: running', file=sys.stderr, flush=True)


def run(pool, priority, gpu, label, command, pool_given=False):
    held = os.environ.get('BB_HEAVY_HELD')
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
        if pool in ('build', 'quick', 'sweep') and (idle_checked is None or time.monotonic() - idle_checked >= 10):
            idle, idle_checked = user_state(), time.monotonic()
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
            waited = time.monotonic() - started
            command, policy = polite(pool, priority, gpu, command, idle)
            if pool in ('build', 'quick', 'sweep') and not priority and policy != 'normal':
                print(f'heavy: {label}: {policy}', file=sys.stderr, flush=True)
            record({'event': 'start', 'label': label, 'pool': pool, 'slot': name, 'priority': priority,
                    'waited_s': round(waited, 1), 'load': [round(x, 2) for x in os.getloadavg()],
                    'held': busy_slots(), 'pid': os.getpid(), 'policy': policy})
            if waited >= 1:
                print(f'heavy: {label}: got {name} after {waited:.0f} s', file=sys.stderr, flush=True)
            os.set_inheritable(fd, True)
            os.environ['BB_HEAVY_HELD'] = f'{name}:{label}'
            if gpu:
                take_gpu(gpu, label)
            announce(label)
            if pool == 'quick':
                supervise_quick(label, command)
            try:
                os.execvp(command[0], command)
            except OSError as error:
                fail(f'cannot run {command[0]}: {error}', 127)
        if time.monotonic() - reported >= WAIT_REPORT_SECONDS:
            held = '; '.join(f'{name}: {describe(holder(name))}' for name in names)
            print(f'heavy: {label}: waiting for a slot ({held})', file=sys.stderr, flush=True)
            reported = time.monotonic()
        time.sleep(1)


def seconds(variable, default):
    value = os.environ.get(variable, str(default))
    if not value.isdigit() or int(value) < 1:
        fail(f'{variable} must be a positive whole number of seconds')
    return int(value)


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
    def finish(status):
        record({'event': 'end', 'label': label, 'pool': 'quick', 'status': status,
                'seconds': round(time.monotonic() - started, 1), 'load': [round(x, 2) for x in os.getloadavg()]})
        sys.exit(status)
    while True:
        try:
            status = child.wait(timeout=1)
            finish(status if status >= 0 else 128 - status)
        except subprocess.TimeoutExpired:
            pass
        elapsed = time.monotonic() - started
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
            finish(124)


def main(argv):
    if argv == ['status']:
        return status()
    pool, priority, gpu, pool_given = 'build', False, None, False
    while argv and argv[0].startswith('--') and argv[0] != '--':
        if argv[0] == '--priority':
            priority, argv = True, argv[1:]
        elif argv[0] == '--pool' and len(argv) > 1 and argv[1] in POOLS:
            pool, argv, pool_given = argv[1], argv[2:], True
        elif argv[0] == '--gpu' and len(argv) > 1 and argv[1] in ('shared', 'exclusive'):
            gpu, argv = argv[1], argv[2:]
        else:
            fail(f'unknown option {argv[0]} (--pool build|playtest|quick|sweep, --priority, --gpu shared|exclusive)')
    if pool == 'playtest' and gpu is None:
        gpu = 'shared'  # a game uses the GPU
    if priority and pool != 'build':
        fail('--priority is for the build pool (shared builds); the playtest pool is dedicated already')
    if len(argv) < 3 or argv[1] != '--' or not argv[0]:
        fail('usage: heavy.sh [--pool build|playtest|quick|sweep] [--priority] [--gpu shared|exclusive] LABEL -- '
             'COMMAND [ARG...] | heavy.sh status')
    run(pool, priority, gpu, argv[0], argv[2:], pool_given)


if __name__ == '__main__':
    main(sys.argv[1:])
