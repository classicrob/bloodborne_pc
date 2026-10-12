#!/usr/bin/env python3
"""Usage: python3 tools/macos/replay_watch.py LOG FIRST QUIET -- cmd...
Runs a command, copying its output to LOG; kills that one child (never by pattern) when it prints
nothing within FIRST seconds of launch, or nothing for QUIET seconds later. Exit: the child's
status, 124 after a watchdog kill."""
import os, signal, subprocess, sys, threading, time
log, first, quiet = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
cmd = sys.argv[sys.argv.index('--') + 1:]
start = time.time(); last = [None]
with open(log, 'wb') as out:
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    def pump():
        for line in iter(p.stdout.readline, b''):
            out.write(line); out.flush(); last[0] = time.time()
    t = threading.Thread(target=pump, daemon=True); t.start()
    killed = None
    while p.poll() is None:
        time.sleep(1)
        now = time.time()
        if last[0] is None and now - start > first:
            killed = f'no output within {first:.0f} s of launch'
        elif last[0] is not None and now - last[0] > quiet:
            killed = f'no output for {quiet:.0f} s'
        if killed:
            p.send_signal(signal.SIGTERM); time.sleep(5)
            if p.poll() is None: p.kill()
            break
    p.wait(); t.join(timeout=5)
    if killed:
        msg = f'WATCHDOG: killed pid {p.pid}: {killed}\n'.encode(); out.write(msg); sys.stderr.write(msg.decode())
        sys.exit(124)
    sys.exit(p.returncode if p.returncode >= 0 else 128 - p.returncode)
