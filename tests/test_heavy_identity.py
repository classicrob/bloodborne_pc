"""heavy.py's CPU quiet resumes and restores only the processes it stopped or demoted: each is
remembered as (pid, start time), so a pid reused by another process is left alone. Real
processes; the remembered start is the seam (a wrong start stands for a reused pid)."""
import importlib.util
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from paths import ROOT

if sys.platform != 'darwin':
    raise unittest.SkipTest('macOS process start times and darwin background QoS')


def load_heavy():
    spec = importlib.util.spec_from_file_location('heavy', ROOT / 'tools' / 'macos' / 'heavy.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ps(field, pid):
    return subprocess.run(['ps', '-o', f'{field}=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()


class ProcessIdentityTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = os.environ.get('BB_HEAVY_DIR')
        os.environ['BB_HEAVY_DIR'] = self.dir.name
        self.heavy = load_heavy()
        self.process = subprocess.Popen(['sleep', '30'])
        time.sleep(0.1)

    def tearDown(self):
        self.process.kill()
        self.process.wait()
        if self.saved is None:
            os.environ.pop('BB_HEAVY_DIR', None)
        else:
            os.environ['BB_HEAVY_DIR'] = self.saved
        self.dir.cleanup()

    def test_process_start(self):
        start = self.heavy.process_start(self.process.pid)
        self.assertIsNotNone(start)
        self.assertLess(abs(start[0] + start[1] / 1e6 - time.time()), 5)
        self.assertEqual(start, self.heavy.process_start(self.process.pid))
        gone = subprocess.Popen(['true'])
        gone.wait()
        self.assertIsNone(self.heavy.process_start(gone.pid))

    def test_restore_only_the_same_process(self):
        pid = self.process.pid
        normal = int(ps('pri', pid))
        os.setpriority(4, pid, 0x1000)  # PRIO_DARWIN_PROCESS, PRIO_DARWIN_BG
        self.assertEqual(ps('pri', pid), '4')
        self.heavy.restore_demoted('T', {pid: (999999, (1, 0))}, {})  # remembered start: another process
        self.assertEqual(ps('pri', pid), '4', 'a reused pid was restored')
        self.heavy.restore_demoted('T', {pid: (999999, self.heavy.process_start(pid))}, {})
        self.assertEqual(int(ps('pri', pid)), normal)

    def test_resume_only_the_same_process(self):
        pid = self.process.pid
        os.kill(pid, signal.SIGSTOP)
        time.sleep(0.1)
        self.assertTrue(ps('stat', pid).startswith('T'))
        self.heavy.resume_paused('T', [('job', [(pid, (1, 0))], 999999)])
        time.sleep(0.1)
        self.assertTrue(ps('stat', pid).startswith('T'), 'a reused pid was resumed')
        self.heavy.resume_paused('T', [('job', [(pid, self.heavy.process_start(pid))], 999999)])
        time.sleep(0.1)
        self.assertFalse(ps('stat', pid).startswith('T'))


if __name__ == '__main__':
    unittest.main()
