"""A browser helper writing into the profile after the application exited must not fail the cleanup (#642)."""
import errno
import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import AutomationClient  # noqa: E402


class ProfileCleanup(unittest.TestCase):
    def test_late_write_is_retried(self):
        path = pathlib.Path(tempfile.mkdtemp(prefix="wf-profile-test-"))
        (path / "cache").mkdir()
        calls = []

        def remove(target):
            calls.append(target)
            if len(calls) == 1:
                # The helper writes after the walk of the directory and before its removal.
                (path / "cache" / "late").write_text("x")
                raise OSError(errno.ENOTEMPTY, "Directory not empty", str(target))
            shutil.rmtree(target)

        try:
            self.assertTrue(AutomationClient.remove_profile(path, delay=0, remove=remove))
            self.assertGreater(len(calls), 1)
            self.assertFalse(path.exists())
        finally:
            shutil.rmtree(path, ignore_errors=True)

    def test_profile_is_outside_the_scenario_directory(self):
        with tempfile.TemporaryDirectory() as scenario, AutomationClient.profile_directory() as profile:
            self.assertFalse(pathlib.Path(profile).resolve().is_relative_to(pathlib.Path(scenario).resolve()))
        self.assertFalse(pathlib.Path(profile).exists())

    def test_missing_directory_is_removed(self):
        self.assertTrue(AutomationClient.remove_profile(pathlib.Path(tempfile.gettempdir()) / "wf-profile-absent-642"))


if __name__ == "__main__":
    unittest.main()
