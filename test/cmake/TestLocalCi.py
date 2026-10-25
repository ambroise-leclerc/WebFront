"""Offline regression tests for local CI transport, command plans and failure reporting."""
import argparse
import io
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "cmake/local-ci"))
import orchestrator as ci
import worker


class LocalCiTests(unittest.TestCase):
    def setUp(self):
        self.matrix = json.loads((ci.HERE / "lanes.json").read_text())
        self.temporary = tempfile.TemporaryDirectory(prefix="wf-ci-test-")
        self.root = Path(self.temporary.name)
        ci.STOP.clear()

    def tearDown(self):
        self.temporary.cleanup()
        ci.STOP.clear()

    def lane(self, name):
        return next(lane for lane in self.matrix["lanes"] if lane["id"] == name)

    def test_selection_and_partial_exit_status(self):
        self.assertEqual(len(ci.selectedLanes(self.matrix, None, None)), 10)
        self.assertEqual(len(ci.selectedLanes(self.matrix, "RyzenLinux", None)), 8)
        for machines, lanes in [("Typo", None), (None, "typo"), ("Mac", "web-integration-gcc")]:
            with self.assertRaises(ValueError):
                ci.selectedLanes(self.matrix, machines, lanes)
        passed = {"status": "passed"}
        for status in ["unreachable", "skipped (no interactive session)"]:
            rows = [passed, {"status": status}]
            self.assertEqual(ci.exitCode(rows, False), 0)
            self.assertEqual(ci.exitCode(rows, True), 1)
        for status in ["failed", "timeout", "interrupted"]:
            self.assertEqual(ci.exitCode([passed, {"status": status}], False), 1)
        self.assertEqual(ci.exitCode([], False), 1)
        self.assertEqual(ci.exitCode([{"status": "unreachable"}], False), 1)

    def test_bundle_contains_unpushed_commit(self):
        source, target = self.root / "source", self.root / "target"
        subprocess.run(["git", "init", str(source)], check=True, capture_output=True)
        (source / "test.txt").write_text("unpushed contents")
        for command in [["git", "add", "test.txt"],
                        ["git", "-c", "user.name=Test", "-c", "user.email=test@example.org", "commit", "-m", "local"],
                        ["git", "bundle", "create", str(self.root / "commit.bundle"), "HEAD"]]:
            subprocess.run(command, cwd=source, check=True, capture_output=True)
        subprocess.run(["git", "clone", str(self.root / "commit.bundle"), str(target)],
                       check=True, capture_output=True)
        self.assertEqual((target / "test.txt").read_text(), "unpushed contents")
        self.assertFalse((source / ".git/config").read_text().find("github") >= 0)

    def test_archive_rejects_escape_links_and_duplicate_files(self):
        for name, kind in [("../escape", tarfile.REGTYPE), ("/escape", tarfile.REGTYPE),
                           ("link", tarfile.SYMTYPE), ("device", tarfile.CHRTYPE),
                           ("a\\b", tarfile.REGTYPE), ("duplicate", tarfile.REGTYPE)]:
            with self.subTest(name=name):
                archive = self.root / "bad.tar"
                with tarfile.open(archive, "w") as stream:
                    member = tarfile.TarInfo(name)
                    member.type = kind
                    stream.addfile(member)
                    if name == "duplicate":
                        stream.addfile(member)
                with self.assertRaises(ValueError):
                    ci.extract(archive, self.root / "out")

    def makeKit(self):
        dependencies = self.root / "deps"
        dependencies.mkdir()
        (dependencies / "preload.cmake").write_text("# offline overrides\n")
        (dependencies / "image.tar").write_bytes(b"image fixture")
        provision = {
            "images": {"linux-ubuntu24-clang23": {"id": "sha256:" + "a" * 64, "archive": "image.tar"}},
            "sources": {"preload.cmake": {"origin": "local overrides", "integrity": "reviewed"},
                        "image.tar": {"origin": "docker save", "integrity": "sha256:" + "a" * 64}}}
        (dependencies / "provision.json").write_text(json.dumps(provision))
        archive = self.root / "kit.tgz"
        ci.pack(argparse.Namespace(dependencies=dependencies, output=archive))
        extracted = self.root / "extracted"
        ci.extract(archive, extracted)
        return archive, extracted

    def test_kit_integrity_and_extra_missing_modified_files(self):
        archive, extracted = self.makeKit()
        self.assertEqual(len(ci.verifyKit(extracted)["images"]), 1)
        target = extracted / "preload.cmake"
        old = target.read_bytes()
        for change in ["extra", "missing", "modified"]:
            with self.subTest(change=change):
                if change == "extra":
                    (extracted / "unexpected").write_text("extra")
                elif change == "missing":
                    target.unlink()
                else:
                    target.write_text("tampered")
                with self.assertRaises(ValueError):
                    ci.verifyKit(extracted)
                (extracted / "unexpected").unlink(missing_ok=True)
                target.write_bytes(old)

    def test_kit_refuses_mutable_image_ids_and_git_metadata(self):
        _, extracted = self.makeKit()
        manifest = json.loads((extracted / "kit.json").read_text())
        manifest["images"]["linux-ubuntu24-clang23"]["id"] = "ubuntu:latest"
        (extracted / "kit.json").write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            ci.verifyKit(extracted)
        (extracted / ".git").mkdir()
        (extracted / ".git/config").write_text("metadata")
        with self.assertRaises(ValueError):
            ci.inventory(extracted)

    def test_all_linux_command_plans_keep_tests_and_filters(self):
        for lane in ci.selectedLanes(self.matrix, "RyzenLinux", None):
            with self.subTest(lane=lane["id"]):
                plan = worker.buildCommands(lane, 3)
                configurations = [cmd for cmd in plan if "-S" in cmd]
                self.assertEqual(len(configurations), len(lane.get("configure_variants", [[]])))
                for cmd in configurations:
                    self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", cmd)
                    self.assertIn("-DWEBFRONT_BOOTSTRAP_DIR=/deps/bootstrap", cmd)
                    self.assertFalse(any("@workspace@" in arg for arg in cmd))
                for cmd in [cmd for cmd in plan if "--build" in cmd]:
                    self.assertEqual(cmd[cmd.index("--parallel") + 1], "3")
                if lane.get("catch2_direct"):
                    tests = [cmd for cmd in plan if cmd[0].endswith("test/unit/tests")]
                    self.assertEqual(len(tests), 6)
                    self.assertEqual(sum("[transport],[weblink]" in cmd for cmd in tests), 5)
                else:
                    tests = [cmd for cmd in plan if cmd[0] == "ctest"]
                    self.assertEqual(len(tests), len(configurations))
                    for cmd in tests:
                        self.assertIn("--no-tests=error", cmd)
                        self.assertIn("--output-on-failure", cmd)
                        for label in lane["ctest"]["labels"]:
                            self.assertEqual(cmd[cmd.index("-L") + 1], label)
        cef = worker.buildCommands(self.lane("web-integration-gcc"), 4)
        build = next(cmd for cmd in cef if "--build" in cmd)
        self.assertEqual(build[build.index("--target") + 1:], ["webtest", "developer_integration", "webfront-wf"])
        fuzz = worker.buildCommands(self.lane("sanitizers-address-undefined"), 4)
        self.assertEqual(sum("-max_total_time=30" in cmd for cmd in fuzz), 4)
        installed = worker.buildCommands(self.lane("mddlog-clang-source-install"), 4)
        self.assertTrue(any("--install" in cmd for cmd in installed))
        self.assertTrue(any("-DCMAKE_PREFIX_PATH=/workspace/mddlog-install" in cmd for cmd in installed))

    def test_linux_command_plans_are_independent_of_host_paths(self):
        for lane in ci.selectedLanes(self.matrix, "RyzenLinux", None):
            with self.subTest(lane=lane["id"]):
                expected = worker.buildCommands(lane, 3)
                # Model Windows path semantics on every CI host. Paths in these
                # commands belong to the Linux container, never to the caller.
                with patch.object(worker, "Path", PureWindowsPath):
                    actual = worker.buildCommands(lane, 3)
                self.assertEqual(actual, expected)
                self.assertFalse(any("\\" in arg for cmd in actual for arg in cmd))

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "CMake and CTest required")
    def test_ctest_subconfiguration_reuses_offline_cpm_module(self):
        cache = self.root / "cache/cpm"
        cache.mkdir(parents=True)
        version = (ROOT / "cmake/CPM.cmake").read_text().split("set(CPM_DOWNLOAD_VERSION ", 1)[1].split(")", 1)[0]
        (cache / f"CPM_{version}.cmake").write_text("set(WF_CPM_LOADED TRUE)\n")
        child = self.root / "child.cmake"
        child.write_text('''# A download attempt is a failure, without ever accessing the network.
macro(file operation)
    if("${operation}" STREQUAL "DOWNLOAD")
        message(FATAL_ERROR "CPM download attempted in an offline subconfiguration")
    endif()
    _file(${ARGV})
endmacro()
''' + f'''include("{(ROOT / 'cmake/CPM.cmake').as_posix()}")
if(NOT WF_CPM_LOADED)
    message(FATAL_ERROR "The prepared CPM module was not loaded")
endif()
''')
        (self.root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.31)
project(OfflineCPM NONE)
enable_testing()
add_test(NAME ChildConfiguration COMMAND "${{CMAKE_COMMAND}}" -P "{child.as_posix()}")
''')
        build = self.root / "build"
        subprocess.run(["cmake", "-S", str(self.root), "-B", str(build)],
                       check=True, capture_output=True, text=True, timeout=20)
        lane = dict(self.lane("web-integration-gcc"), options=["-DWEBFRONT_EMBED_CEF=OFF"])

        def execute(invocation, **kwargs):
            if invocation[0] == "ctest":
                env = dict(kwargs["env"])
                # Map the container mount to this test's prepared host cache.
                if env.get("CPM_SOURCE_CACHE") == "/cache/cpm":
                    env["CPM_SOURCE_CACHE"] = str(cache.parent)
                result = subprocess.run(invocation, env=env, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result
            return subprocess.CompletedProcess(invocation, 0, "cmake version " + lane["cmake_version"])

        # The fixture uses the host's default generator, including multi-config Visual Studio.
        # CTest needs a configuration there even though this project builds no compiler targets.
        plan = [["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure", "--no-tests=error"]]
        with patch.dict(os.environ), patch.object(worker.os, "umask"), \
             patch.object(worker, "command", side_effect=execute), \
             patch.object(worker, "buildCommands", return_value=plan):
            os.environ.pop("CPM_SOURCE_CACHE", None)
            worker.executeInside(lane, 1)

    @unittest.skipUnless(os.name == "posix", "Linux container worker")
    def test_linux_container_is_offline_unprivileged_and_stopped(self):
        lane = self.lane("web-integration-gcc")
        self.root = self.root / ".webfront-local-ci/runs/run"
        self.root.mkdir(parents=True)
        (self.root / "dependencies").mkdir()
        (self.root / "dependencies/kit.json").write_text(json.dumps({"images": {
            "linux-ubuntu24-clang23": {"id": "sha256:" + "a" * 64}}}))
        workspace = self.root / lane["id"]
        args = argparse.Namespace(lane=lane["id"], sha="b" * 40, jobs=2, timeout=10, keep=False)
        with patch.object(worker, "ROOT", self.root), patch.object(worker, "command") as commands, \
             patch.object(worker, "runProcess", side_effect=subprocess.CalledProcessError(1, "test")) as run, \
             patch.object(worker.subprocess, "run") as stop:
            with self.assertRaises(subprocess.CalledProcessError):
                worker.runLane(args, self.matrix)
            invocation = run.call_args.args[0]
            for flag in ["--network=none", "--pull=never", "--cap-drop=ALL", "--security-opt=no-new-privileges", "--user"]:
                self.assertIn(flag, invocation)
            self.assertNotIn("--privileged", invocation)
            self.assertEqual(stop.call_count, 2)
            self.assertTrue((self.root / (lane["id"] + "-artifacts.tar")).is_file())
            self.assertFalse(workspace.exists())
        args.keep = True
        with patch.object(worker, "ROOT", self.root), patch.object(worker, "command"), \
             patch.object(worker, "runProcess", side_effect=subprocess.CalledProcessError(1, "test")), \
             patch.object(worker.subprocess, "run") as stop:
            with self.assertRaises(subprocess.CalledProcessError):
                worker.runLane(args, self.matrix)
            stop.assert_called_once()
            self.assertTrue(workspace.exists())

    @unittest.skipUnless(os.name == "posix", "Linux container permissions")
    def test_cleanup_uses_test_identity_for_private_profiles(self):
        workspace = self.root / "workspace"
        profile = workspace / "profile"
        profile.mkdir(parents=True, mode=0o700)
        (profile / "data").write_text("private browser data")
        outside = self.root / "outside"
        outside.write_text("untouched")
        (workspace / "link").symlink_to(outside)
        originalRun = subprocess.run

        def clean(invocation, **kwargs):
            for flag in ("--network=none", "--pull=never", "--cap-drop=ALL", "--group-add=0"):
                self.assertIn(flag, invocation)
            self.assertEqual(invocation[invocation.index("--user") + 1], f"{os.getuid()}:{os.getgid()}")
            self.assertEqual(invocation.count("--mount"), 1)
            script = invocation[-1].replace("Path('/workspace')", "Path(" + repr(str(workspace)) + ")")
            return originalRun([sys.executable, "-c", script], check=True, timeout=kwargs["timeout"])

        with patch.object(worker, "command", side_effect=clean), \
             patch.object(worker.subprocess, "run") as stop, \
             patch.object(worker.shutil, "rmtree", side_effect=PermissionError("host cannot read private profile")):
            worker.cleanWorkspace(workspace, "sha256:" + "a" * 64, "fixture")
            stop.assert_called_once()
        self.assertFalse(workspace.exists())
        self.assertEqual(outside.read_text(), "untouched")

    def test_tsan_host_requirement_stops_before_docker(self):
        lane = self.lane("sanitizers-thread")
        args = argparse.Namespace(lane=lane["id"])
        with patch.object(worker, "command", return_value=subprocess.CompletedProcess([], 0, "32\n")) as cmd:
            with self.assertRaisesRegex(ValueError, r"vm.mmap_rnd_bits=28"):
                worker.runLane(args, self.matrix)
            cmd.assert_called_once_with(["sysctl", "-n", "vm.mmap_rnd_bits"], capture_output=True, text=True)

    def test_failure_timeout_and_interruption_are_nonzero(self):
        with tempfile.TemporaryFile() as log:
            with self.assertRaises(subprocess.CalledProcessError):
                ci.runProcess([sys.executable, "-c", "raise SystemExit(7)"], log, 2)
            with self.assertRaises(TimeoutError):
                ci.runProcess([sys.executable, "-c", "import time; time.sleep(30)"], log, 0.1)
            ci.STOP.set()
            with self.assertRaises(InterruptedError):
                ci.runProcess([sys.executable, "-c", "import time; time.sleep(30)"], log, 2)

    @unittest.skipUnless(os.name == "posix" and os.getuid() != 0, "Dedicated non-root POSIX worker")
    def test_orchestrator_transport_failure_and_unreachable_machine(self):
        import shutil
        repository = self.root / "repository"
        repository.mkdir()
        scripts = repository / "cmake/local-ci"
        shutil.copytree(ci.HERE, scripts)
        subprocess.run(["git", "init", str(repository)], check=True, capture_output=True)
        (repository / ".gitignore").write_text("build*/\n")
        for invocation in [["git", "add", "."],
                           ["git", "-c", "user.name=Test", "-c", "user.email=test@example.org",
                            "commit", "-m", "fixture"]]:
            subprocess.run(invocation, cwd=repository, check=True, capture_output=True)
        archive, _ = self.makeKit()
        binaries = self.root / "bin"
        binaries.mkdir()
        remoteRoot = self.root / "remote"
        remoteRoot.mkdir()
        # Exercise the real tar/bundle/worker protocol with local SSH and Docker stand-ins.
        (binaries / "ssh").write_text("#!" + sys.executable + "\n" + r'''
import os, pathlib, shlex, subprocess, sys
if os.environ.get("WF_TEST_UNREACHABLE"):
    raise SystemExit(255)
args = shlex.split(sys.argv[-1])
root = pathlib.Path(os.environ["WF_TEST_REMOTE"])
if args[:2] == ["python3", "-c"]:
    args[2] = args[2].replace("pathlib.Path.home()", "pathlib.Path(" + repr(str(root)) + ")")
raise SystemExit(subprocess.run(args, cwd=root).returncode)
''')
        (binaries / "docker").write_text("#!" + sys.executable + "\n" + r'''
import os, subprocess, sys
with open(os.environ["WF_TEST_DOCKER_LOG"], "a") as log:
    log.write(repr(sys.argv[1:]) + "\n")
if sys.argv[1] == "run":
    workspace = next(arg.split("src=", 1)[1].split(",dst=", 1)[0] for arg in sys.argv
                     if arg.startswith("type=bind,src=") and arg.endswith("dst=/workspace"))
    if sys.argv[-2] == "-c":
        # Cleanup has a single mount and executes a fixed script on that mount.
        script = sys.argv[-1].replace("Path('/workspace')", "Path(" + repr(workspace) + ")")
        exec(script)
        raise SystemExit(0)
    assert os.path.isdir(os.path.join(workspace, ".git"))
    assert subprocess.check_output(["git", "-C", workspace, "ls-files"]).strip()
    if os.environ.get("WF_TEST_DOCKER_SUCCESS"):
        print("simulated successful container", flush=True)
        raise SystemExit(0)
    print("intentionally broken test", flush=True)
    raise SystemExit(7)
if sys.argv[1] == "info":
    print('["name=rootless"]')
''')
        for path in binaries.iterdir():
            path.chmod(0o755)
        dockerLog = self.root / "docker.log"
        env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ["PATH"],
                   WF_TEST_REMOTE=str(remoteRoot), WF_TEST_DOCKER_LOG=str(dockerLog))
        invocation = [sys.executable, str(scripts / "run"), "run", "--lanes", "web-integration-gcc",
                      "--kit", str(archive), "--kit-sha256", ci.digest(archive), "--require-all"]
        failed = subprocess.run(invocation, cwd=repository, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        results = list(repository.glob("build-local-ci/*/*/results.json"))
        self.assertEqual(len(results), 1)
        row = json.loads(results[0].read_text())["results"][0]
        self.assertEqual(row["status"], "failed")
        self.assertIn("intentionally broken test", (results[0].parent / "web-integration-gcc.log").read_text())
        self.assertTrue((results[0].parent / "web-integration-gcc-artifacts.tar").exists())
        self.assertIn("--network=none", dockerLog.read_text())
        self.assertEqual(list((remoteRoot / ".webfront-local-ci/runs").iterdir()), [])
        env["WF_TEST_DOCKER_SUCCESS"] = "1"
        succeeded = subprocess.run(invocation, cwd=repository, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(succeeded.returncode, 0, succeeded.stdout + succeeded.stderr)
        self.assertIn("passed", succeeded.stdout)
        self.assertIn("INCOMPLETE", succeeded.stdout)
        # Two independent Linux checkouts can run concurrently and keep separate results.
        parallel = list(invocation)
        parallel[parallel.index("--lanes") + 1] = "web-integration-gcc,web-integration-clang"
        concurrent = subprocess.run(parallel, cwd=repository, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(concurrent.returncode, 0, concurrent.stdout + concurrent.stderr)
        self.assertEqual(concurrent.stdout.count("passed"), 2)
        env["WF_TEST_UNREACHABLE"] = "1"
        missing = subprocess.run(invocation, cwd=repository, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(missing.returncode, 1, missing.stdout + missing.stderr)
        self.assertIn("unreachable", missing.stdout)
        # A modified tree is rejected before SSH and --allow-dirty still tests only the commit.
        (repository / "untracked.txt").write_text("local change")
        (scripts / "lanes.json").write_text("dirty, invalid metadata must not be sent")
        dirty = subprocess.run(invocation, cwd=repository, env=env, capture_output=True, text=True, timeout=20)
        self.assertNotEqual(dirty.returncode, 0)
        self.assertIn("Working tree is modified", dirty.stderr)
        allowed = subprocess.run(invocation + ["--allow-dirty"], cwd=repository, env=env,
                                 capture_output=True, text=True, timeout=20)
        self.assertIn("unreachable", allowed.stdout)

    def test_bootstrap_refuses_to_overwrite_existing_run(self):
        # The actual bootstrap protocol is a tar stream, independent of a remote GitHub clone.
        script = ci.BOOTSTRAP.replace('pathlib.Path.home()', repr(str(self.root)))
        script = script.replace(repr(str(self.root)) + '/".webfront-local-ci"',
                                'pathlib.Path(' + repr(str(self.root)) + ')/".webfront-local-ci"')
        payload = io.BytesIO()
        with tarfile.open(fileobj=payload, mode="w") as archive:
            data = b"worker fixture"
            info = tarfile.TarInfo("worker.py")
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        first = subprocess.run([sys.executable, "-c", script, "run-id"], input=payload.getvalue(), capture_output=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        second = subprocess.run([sys.executable, "-c", script, "run-id"], input=payload.getvalue(), capture_output=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual((self.root / ".webfront-local-ci/runs/run-id/worker.py").read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
