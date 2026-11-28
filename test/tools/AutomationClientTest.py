"""Port-collision status can arrive just after discovery fails; each launch has its own profiles and names itself on failure."""
import pathlib
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


class LaunchIsolation(unittest.TestCase):
    def test_each_launch_has_its_own_webview2_profile(self):
        # Without it every automation_host shares WebView2's default user data folder, hence one browser process (#720).
        first = AutomationClient.host_environment({"PATH": "x"}, pathlib.Path("one") / "profile", "one")
        second = AutomationClient.host_environment({"PATH": "x"}, pathlib.Path("two") / "profile", "two")
        self.assertEqual(first["WEBFRONT_TEST_WEBVIEW2_PROFILE"], str(pathlib.Path("one") / "profile" / "webview2"))
        for name in ("WEBFRONT_TEST_WEBVIEW2_PROFILE", "WEBFRONT_TEST_CEF_PROFILE", "XDG_DATA_HOME", "XDG_CACHE_HOME", "TMP"):
            self.assertNotEqual(first[name], second[name], name)
        self.assertEqual(first["PATH"], "x")

    def test_launch_failure_names_the_launch_and_carries_the_log(self):
        report = AutomationClient.launch_failure("stable-0-1", 1, 4242, ConnectionResetError(10054, "reset"), 1,
                                                 "--- stable-0-1 ---\nautomation_host: listening\n")
        self.assertIn("stable-0-1: launch 2 of 2 on the fixed MCP port 4242 failed: ConnectionResetError", report)
        self.assertIn("host exit status: 1", report)
        self.assertIn("--- stable.log ---\n--- stable-0-1 ---\nautomation_host: listening", report)


if __name__ == "__main__":
    unittest.main()
