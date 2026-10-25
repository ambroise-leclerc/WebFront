"""Offline Windows orchestration regressions; no account/firewall/task changes."""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "cmake/local-ci"))
import orchestrator as ci
import windows_worker as windows


class WindowsCiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="wf-native-test-")
        self.root = Path(self.temporary.name) / ".webfront-local-ci/runs/run"
        self.root.mkdir(parents=True)
        self.matrix = json.loads((ci.HERE / "lanes.json").read_text())
        self.lane = next(row for row in self.matrix["lanes"] if row["runtime"] == "native:msvc")
        self.args = argparse.Namespace(lane=self.lane["id"], sha="a" * 40, jobs=12, timeout=1, keep=False)
        ci.STOP.clear()

    def tearDown(self):
        ci.STOP.clear()
        self.temporary.cleanup()

    def test_windows_transport_uses_encoded_powershell_literals(self):
        values = ["python.exe", "path with spaces/worker.py", "O'Brien;$HOME&|%", "\"quoted\""]
        invocation = ci.remote("CurmudgeonWindows", values)
        encoded = invocation[-1].split()[-1]
        script = base64.b64decode(encoded).decode("utf-16-le")
        self.assertIn("Set-Location $HOME; & 'python.exe'", script)
        self.assertIn("'O''Brien;$HOME&|%'", script)
        self.assertTrue(script.endswith("exit $LASTEXITCODE"))
        self.assertIn("-EncodedCommand", invocation[-1])

    def test_windows_plan_preserves_metadata_and_caps_compilations(self):
        plan = windows.buildCommands(self.lane, 12, "C:/CI user/work", "C:/CI user/deps", "C:/CI user/cache")
        configure, build, test = plan
        for option in self.lane["common_options"] + self.lane["options"]:
            self.assertIn(option, configure)
        self.assertIn("-DCMAKE_CXX_COMPILER=cl", configure)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", configure)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", configure)
        self.assertIn("-DWEBFRONT_BOOTSTRAP_DIR=C:/CI user/deps/bootstrap", configure)
        self.assertEqual(build[build.index("--parallel") + 1], "4")
        self.assertEqual(test[test.index("-C") + 1], "Release")
        self.assertIn("--no-tests=error", test)
        self.assertIn("--output-junit", test)
        self.assertEqual(windows.buildCommands(self.lane, 2, "C:/w", "C:/d", "C:/c")[1][-1], "2")

    def test_task_uses_interactive_limited_principal_and_bounded_lifetime(self):
        with patch.object(windows, "ps") as run:
            windows.startTask(self.root, self.args)
        script = run.call_args.args[0]
        self.assertIn("-LogonType Interactive -RunLevel Limited", script)
        self.assertIn("-ExecutionTimeLimit", script)
        self.assertIn("Start-ScheduledTask", script)
        self.assertNotIn("-Password", script)
        self.assertNotIn("-Force", script)
        self.assertIn("--jobs 4", script)

    def test_windows_kit_needs_no_docker_but_keeps_integrity(self):
        dependencies = self.root / "input"
        dependencies.mkdir()
        (dependencies / "preload.cmake").write_text("# overrides")
        (dependencies / "provision.json").write_text(json.dumps({"platform": "windows", "sources": {
            "preload.cmake": {"origin": "fixture", "integrity": "reviewed"}}}))
        archive = self.root / "kit.tgz"
        ci.pack(argparse.Namespace(dependencies=dependencies, output=archive))
        with patch.object(windows, "checkNetwork"), patch.object(windows, "command") as run:
            windows.prepare(self.root, ci.digest(archive))
        self.assertEqual(run.call_args.args[0][:3], ["git", "clone", "--mirror"])
        self.assertEqual(ci.verifyKit(self.root / "dependencies")["platform"], "windows")
        (self.root / "dependencies/preload.cmake").write_text("tampered")
        with self.assertRaises(ValueError):
            ci.verifyKit(self.root / "dependencies")

    def test_bad_transfer_fails_before_extraction_or_policy_probe(self):
        (self.root / "kit.tgz").write_bytes(b"tampered")
        with patch.object(windows, "checkNetwork") as probe:
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                windows.prepare(self.root, "bad")
            probe.assert_not_called()
        self.assertFalse((self.root / "dependencies").exists())

    def test_public_network_check_requires_effective_complete_policy(self):
        valid = subprocess.CompletedProcess([], 0, json.dumps(windows.publicNetworks()))
        with patch.object(windows, "ps", return_value=valid) as run:
            windows.checkNetwork()
            script = run.call_args.args[0]
            self.assertIn("-PolicyStore ActiveStore", script)
            self.assertIn("Get-NetFirewallApplicationFilter", script)
            self.assertIn("Get-NetFirewallInterfaceFilter", script)
        for addresses in [["0.0.0.0/1"], ["0.0.0.0/0"], ["::/0"], []]:
            with patch.object(windows, "ps", return_value=subprocess.CompletedProcess([], 0, json.dumps(addresses))):
                with self.assertRaises(ValueError):
                    windows.checkNetwork()
        with patch.object(windows, "ps", side_effect=subprocess.CalledProcessError(1, "firewall")):
            with self.assertRaises(subprocess.CalledProcessError):
                windows.checkNetwork()

    def test_failed_session_collects_diagnostics_and_removes_task(self):
        workspace = self.root / self.args.lane
        workspace.mkdir()
        (workspace / "test-results.xml").write_text("<testsuite failures='1'/>")
        (self.root / (self.args.lane + ".session.log")).write_text("broken test")
        def launch(*_):
            (self.root / (self.args.lane + ".result.json")).write_text('{"code":7}')
        with patch.object(windows, "startTask", side_effect=launch), patch.object(windows, "stopTask") as stop, \
             patch("sys.stdout", new_callable=io.StringIO):
            with self.assertRaises(subprocess.CalledProcessError):
                windows.runLane(self.root, self.args)
            stop.assert_called_once_with(self.root, self.args.lane)
        with tarfile.open(self.root / (self.args.lane + "-artifacts.tar")) as archive:
            self.assertIn("test-results.xml", archive.getnames())
            self.assertIn(self.args.lane + ".session.log", archive.getnames())
        self.assertFalse(workspace.exists())

    def test_success_and_keep(self):
        workspace = self.root / self.args.lane
        workspace.mkdir()
        self.args.keep = True
        (self.root / (self.args.lane + ".result.json")).write_text('{"code":0}')
        with patch.object(windows, "startTask"), patch.object(windows, "stopTask") as stop:
            windows.runLane(self.root, self.args)
            stop.assert_called_once()
        self.assertTrue(workspace.exists())

    def test_timeout_interruption_and_missing_session_stop_the_task(self):
        for interrupted in (False, True):
            with self.subTest(interrupted=interrupted):
                self.args.timeout = 0.02
                ci.STOP.clear()
                if interrupted:
                    ci.STOP.set()
                with patch.object(windows, "startTask"), patch.object(windows, "stopTask") as stop:
                    with self.assertRaises(InterruptedError if interrupted else TimeoutError):
                        windows.runLane(self.root, self.args)
                    stop.assert_called_once()

    def test_cancellation_signals_and_unregisters_exact_task(self):
        with patch.object(windows, "ps") as run:
            windows.stopTask(self.root, self.args.lane)
        self.assertTrue((self.root / (self.args.lane + ".cancel")).exists())
        self.assertIn(windows.taskName(self.root, self.args.lane), run.call_args.args[0])
        self.assertIn("Unregister-ScheduledTask", run.call_args.args[0])
        self.assertNotIn("-Force", run.call_args.args[0])

    def test_cleanup_refuses_unrelated_directory(self):
        self.args.action = "cleanup"
        self.args.lane = None
        with self.assertRaisesRegex(ValueError, "Refusing cleanup"):
            windows.dispatch(self.args, self.matrix, Path(self.temporary.name))
        self.assertTrue(self.root.exists())

    def test_session_rejects_unavailable_desktop_before_configure(self):
        with patch.object(windows, "ownProcessTree"), \
             patch.object(windows, "checkDesktop", side_effect=ValueError("locked desktop")), \
             patch.object(windows, "command") as run, patch.object(sys, "stdout"), patch.object(sys, "stderr"):
            self.assertEqual(windows.session(self.root, self.args, self.lane), 1)
            run.assert_not_called()
        result = self.root / (self.args.lane + ".result.json")
        self.assertEqual(json.loads(result.read_text())["code"], 1)
        self.assertIn("locked desktop", (self.root / (self.args.lane + ".session.log")).read_text())

    def test_native_session_runs_planned_commands_and_propagates_build_failure(self):
        def command(invocation, **kwargs):
            return subprocess.CompletedProcess(invocation, 0, "cmake version " + self.lane["cmake_version"] + "\n")
        for failed in (False, True):
            with self.subTest(failed=failed), patch.object(windows, "ownProcessTree"), \
                 patch.object(windows, "checkDesktop"), patch.object(windows, "checkNetwork"), \
                 patch.object(windows, "checkRuntime"), patch.object(windows, "msvcEnvironment", return_value={}), \
                 patch.object(windows, "command", side_effect=command), patch.dict(os.environ), \
                 patch.object(sys, "stdout"), patch.object(sys, "stderr"), \
                 patch.object(windows, "runProcess", side_effect=(
                     [None, subprocess.CalledProcessError(7, "build")] if failed else None)) as run:
                self.assertEqual(windows.session(self.root, self.args, self.lane), int(failed))
                calls = [call.args[0] for call in run.call_args_list]
                self.assertEqual(len(calls), 2 if failed else 3)
                self.assertIn("--build", calls[1])
                if not failed:
                    self.assertEqual(calls[2][0], "ctest")
                result = json.loads((self.root / (self.args.lane + ".result.json")).read_text())
                self.assertEqual(result["code"], int(failed))

    @unittest.skipUnless(os.name == "nt", "Native Windows transport")
    def test_real_powershell_preserves_binary_stdin_stdout_and_arguments(self):
        invocation = ci.remote("CurmudgeonWindows", [sys.executable, "-c",
                               "import sys;sys.stdout.buffer.write(sys.stdin.buffer.read());"
                               "assert sys.argv[1] == \"O'Brien & space\"", "O'Brien & space"])
        encoded = invocation[-1].split()[-1]
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                                input=bytes(range(256)), capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, bytes(range(256)))

    def test_full_windows_transport_with_native_session_standin(self):
        repository = Path(self.temporary.name) / "repository"
        scripts = repository / "cmake/local-ci"
        shutil.copytree(ci.HERE, scripts)
        (repository / ".gitignore").write_text("build*/\n__pycache__/\n")
        (repository / "unpublished.txt").write_text("local commit")
        subprocess.run(["git", "init", str(repository)], check=True, capture_output=True)
        for invocation in [["git", "add", "."], ["git", "-c", "user.name=Test", "-c",
                           "user.email=test@example.org", "commit", "-m", "local fixture"]]:
            subprocess.run(invocation, cwd=repository, check=True, capture_output=True)
        dependencies = Path(self.temporary.name) / "prepared"
        dependencies.mkdir()
        (dependencies / "preload.cmake").write_text("# fixture")
        (dependencies / "provision.json").write_text(json.dumps({"platform": "windows", "sources": {
            "preload.cmake": {"origin": "fixture", "integrity": "reviewed"}}}))
        archive = Path(self.temporary.name) / "kit.tgz"
        ci.pack(argparse.Namespace(dependencies=dependencies, output=archive))
        remoteRoot = Path(self.temporary.name) / "remote account"
        remoteRoot.mkdir()
        shim = Path(self.temporary.name) / "ssh.py"
        shim.write_text('''import pathlib,sys,runpy,json,subprocess
sys.dont_write_bytecode=True
root=pathlib.Path(sys.argv[1])
args=sys.argv[2:]
if args[1]=='-c':
    pathlib.Path.home=classmethod(lambda cls: root)
    sys.argv=['-c']+args[3:]
    exec(args[2])
else:
    worker=root/args[1]
    sys.path.insert(0,str(worker.parent))
    import windows_worker as w
    w.checkNetwork=lambda: None
    def launch(root,args):
        workspace=root/args.lane
        subprocess.run(['git','clone',str(root/'repo.git'),str(workspace)],check=True)
        subprocess.run(['git','-C',str(workspace),'checkout','--detach',args.sha],check=True)
        assert (workspace/'unpublished.txt').read_text()=='local commit'
        (workspace/'test-results.xml').write_text('<testsuite/>')
        (root/(args.lane+'.session.log')).write_text('native session fixture')
        (root/(args.lane+'.result.json')).write_text(json.dumps({'code':int(__import__('os').environ['WF_TEST_CODE'])}))
    w.startTask=launch
    w.stopTask=lambda *args: None
    sys.argv=[str(worker)]+args[2:]
    # Exercise the native dispatch on POSIX too, without changing os.name globally.
    import worker as shared,argparse
    p=argparse.ArgumentParser()
    p.add_argument('action')
    for option in ['lane','sha','kit-sha256']: p.add_argument('--'+option)
    for option in ['jobs','timeout']: p.add_argument('--'+option,type=int)
    p.add_argument('--keep',action='store_true')
    parsed=p.parse_args(args[2:])
    try: code=w.dispatch(parsed,json.loads((worker.parent/'lanes.json').read_text()),worker.parent)
    except (ValueError,OSError,subprocess.SubprocessError): code=1
    raise SystemExit(code)
''', encoding="utf-8")
        def remote(machine, arguments):
            self.assertEqual(machine, "CurmudgeonWindows")
            return [sys.executable, str(shim), str(remoteRoot), *arguments]
        args = argparse.Namespace(allow_dirty=False, commit="HEAD", machines="CurmudgeonWindows", lanes=None,
                                  kit=None, kit_sha256=None, windows_kit=archive, windows_kit_sha256=ci.digest(archive),
                                  jobs=4, timeout=20, transfer_timeout=20, parallel_lanes=1, keep=False, require_all=True)
        previous = Path.cwd()
        try:
            os.chdir(repository)
            for code in (0, 7):
                with patch.object(ci, "remote", side_effect=remote), \
                     patch.object(ci, "ssh", return_value=[sys.executable, "-c", "print('reachable')"]), \
                     patch.dict(os.environ, WF_TEST_CODE=str(code)), patch("sys.stdout", new_callable=io.StringIO):
                    actual = ci.execute(args)
                    logs = "\n".join(path.read_text(errors="replace") for path in repository.glob("build-local-ci/*/*/*.log"))
                    self.assertEqual(actual, int(bool(code)), logs)
            results = [json.loads(path.read_text()) for path in repository.glob("build-local-ci/*/*/results.json")]
            self.assertEqual({row["results"][0]["status"] for row in results}, {"passed", "failed"})
            for artifact in repository.glob("build-local-ci/*/*/*-artifacts.tar"):
                with tarfile.open(artifact) as stream:
                    self.assertIn("test-results.xml", stream.getnames())
            self.assertEqual(list((remoteRoot / ".webfront-local-ci/runs").iterdir()), [])
        finally:
            os.chdir(previous)

    @unittest.skipUnless(os.name == "nt", "Windows Job Object")
    def test_killing_worker_also_kills_child_process(self):
        script = ("import sys,subprocess,time; sys.path.insert(0," + repr(str(ci.HERE)) + "); "
                  "import windows_worker as w; job=w.ownProcessTree(); "
                  "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); "
                  "print(p.pid,flush=True); time.sleep(60)")
        parent = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE, text=True)
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = None
        try:
            child = int(parent.stdout.readline())
            handle = kernel.OpenProcess(0x100000, False, child)
            self.assertTrue(handle)
            parent.terminate()
            parent.wait(timeout=10)
            self.assertEqual(kernel.WaitForSingleObject(handle, 10000), 0)
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=10)
            parent.stdout.close()
            if handle:
                kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
