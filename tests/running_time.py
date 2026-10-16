"""run(): subprocess.run with a time limit that a SIGSTOP of the test does not use up.

heavy.sh --cpu-quiet SIGSTOPs build and test jobs for the length of a perf run; a plain timeout
kept counting through the stop and failed the test as soon as it resumed. Here monotonic time is
added up per poll with each observed gap clamped to half a second: an approximation of running
time, not a measurement of stopped time. A stop of any length (the test is stopped with the
child) costs at most 0.5 s; so does any other stall longer than that. Same arguments as subprocess.run (capture_output, text, input, check, env,
cwd, ...); raises subprocess.TimeoutExpired at the limit, after killing the child."""
import subprocess
import time

STEP, MOST = 0.1, 0.5


def run(command, seconds, *, input=None, capture_output=False, check=False, **popen):
    if capture_output:
        popen.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if input is not None:
        popen['stdin'] = subprocess.PIPE
    counted, last = 0.0, time.monotonic()
    with subprocess.Popen(command, **popen) as process:
        pending = input
        while True:
            try:
                out, err = process.communicate(pending, timeout=STEP)
                break
            except subprocess.TimeoutExpired:
                pending = None  # sent with the first call; retrying keeps the output read so far
                now = time.monotonic()
                counted, last = counted + min(now - last, MOST), now
                if counted >= seconds:
                    process.kill()
                    out, err = process.communicate()
                    raise subprocess.TimeoutExpired(command, seconds, out, err)
    result = subprocess.CompletedProcess(command, process.returncode, out, err)
    if check:
        result.check_returncode()
    return result
