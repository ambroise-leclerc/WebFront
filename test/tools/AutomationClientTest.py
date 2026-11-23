"""Port-collision status can arrive just after discovery fails."""
import subprocess
import sys
import unittest

import AutomationClient


class PortConflictRetry(unittest.TestCase):
    def test_waits_for_late_collision_exit(self):
        with subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.read(1); raise SystemExit(73)"], stdin=subprocess.PIPE) as process:
            self.assertIsNone(process.poll())
            process.stdin.write(b"x")
            process.stdin.flush()
            self.assertTrue(AutomationClient.port_conflict_after_failure(process, timeout=5))
            self.assertEqual(process.returncode, 73)

    def test_other_exit_is_not_retried(self):
        with subprocess.Popen([sys.executable, "-c", "raise SystemExit(1)"]) as process:
            self.assertFalse(AutomationClient.port_conflict_after_failure(process, timeout=5))
            self.assertEqual(process.returncode, 1)

    def test_still_running_is_not_retried(self):
        with subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"]) as process:
            try:
                self.assertFalse(AutomationClient.port_conflict_after_failure(process, timeout=0.05))
            finally:
                process.terminate()


if __name__ == "__main__":
    unittest.main()
