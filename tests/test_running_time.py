"""running_time.run: a time limit that a SIGSTOP of the test does not use up (heavy.sh --cpu-quiet)."""
import os
import signal
import subprocess
import sys
import time
import unittest

import running_time
from paths import ROOT


class RunningTimeTests(unittest.TestCase):
    def test_result_like_subprocess_run(self):
        r = running_time.run([sys.executable, '-c', 'import sys; print(sys.stdin.read().upper()); sys.exit(3)'],
                             10, input='abc', capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout), (3, 'ABC\n'))
        with self.assertRaises(subprocess.CalledProcessError):
            running_time.run([sys.executable, '-c', 'raise SystemExit(4)'], 10, check=True)

    def test_limit_kills_the_child(self):
        start = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            running_time.run([sys.executable, '-c', 'import time; time.sleep(30)'], 1, capture_output=True)
        self.assertLess(time.monotonic() - start, 5)

    def test_a_stop_of_the_test_does_not_count(self):
        # A test process with a 1.5 s limit on a 1 s child, stopped (with the child) for 3 s.
        # A wall-clock timeout fires on resume; running time does not.
        code = ('import sys, running_time; sys.exit(running_time.run(["sleep", "1"], 1.5).returncode)')
        process = subprocess.Popen([sys.executable, '-c', code], cwd=ROOT / 'tests', start_new_session=True,
                                   stderr=subprocess.PIPE, text=True)
        time.sleep(0.4)
        os.killpg(process.pid, signal.SIGSTOP)
        time.sleep(3)
        os.killpg(process.pid, signal.SIGCONT)
        _, err = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 0, err)


if __name__ == '__main__':
    unittest.main()
