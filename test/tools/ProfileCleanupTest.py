"""A browser helper writing into the profile after the application exited must not fail the cleanup (#642)."""
import errno
import pathlib
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

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

    def test_vanished_file_is_not_a_vanished_root(self):
        path = pathlib.Path(tempfile.mkdtemp(prefix="wf-profile-test-"))
        calls = []

        def remove(target):
            calls.append(target)
            if len(calls) == 1:
                # shutil.rmtree reports a file that a helper removed during its walk, while the root remains.
                raise FileNotFoundError(errno.ENOENT, "No such file", str(target / "gone"))
            shutil.rmtree(target)

        try:
            self.assertTrue(AutomationClient.remove_profile(path, delay=0, remove=remove))
            self.assertGreater(len(calls), 1)
            self.assertFalse(path.exists())
        finally:
            shutil.rmtree(path, ignore_errors=True)

    def test_remaining_root_is_reported(self):
        path = pathlib.Path(tempfile.mkdtemp(prefix="wf-profile-test-"))
        try:
            def remove(target):
                raise FileNotFoundError(errno.ENOENT, "No such file", str(target / "gone"))

            with mock.patch.object(AutomationClient.shutil, "rmtree"):
                self.assertFalse(AutomationClient.remove_profile(path, attempts=2, delay=0, remove=remove))
        finally:
            shutil.rmtree(path, ignore_errors=True)

    def test_host_profiles_are_outside_the_scenario_directory(self):
        with tempfile.TemporaryDirectory() as scenario, tempfile.TemporaryFile("w+") as log:
            root = pathlib.Path(scenario).resolve()
            args = types.SimpleNamespace(host="automation_host", environment="environment.tgz", registry="registry")
            with mock.patch.object(AutomationClient.subprocess, "Popen") as popen:
                popen.return_value.poll.return_value = 0
                host = AutomationClient.Host(args, root / "project", root, "blocked", log, integrity="sha512-x")
                env = popen.call_args.kwargs["env"]
                try:
                    profiles = [pathlib.Path(env[name]).resolve()
                                for name in ("XDG_DATA_HOME", "XDG_CACHE_HOME", "WEBFRONT_TEST_CEF_PROFILE")]
                    private = pathlib.Path(env["TMPDIR"]).resolve()
                    for profile in profiles:
                        self.assertFalse(profile.is_relative_to(root), profile)
                        self.assertTrue(profile.is_relative_to(private), profile)
                finally:
                    host.close()
            self.assertFalse(private.exists())

    def test_missing_directory_is_removed(self):
        self.assertTrue(AutomationClient.remove_profile(pathlib.Path(tempfile.gettempdir()) / "wf-profile-absent-642"))


if __name__ == "__main__":
    unittest.main()
