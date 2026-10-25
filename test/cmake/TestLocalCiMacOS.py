"""Offline macOS driver regressions; no real account/LaunchAgent changes."""
import argparse
import ctypes
import errno
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/local-ci'))
import orchestrator as ci
import macos_worker as mac


class MacCiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='wf-mac-test-')
        self.root = Path(self.temporary.name) / '.webfront-local-ci/runs/run'
        self.root.mkdir(parents=True)
        self.matrix = json.loads((ci.HERE / 'lanes.json').read_text())
        self.lane = next(row for row in self.matrix['lanes'] if row['machine'] == 'Mac')
        self.args = argparse.Namespace(lane=self.lane['id'], sha='a' * 40, jobs=12, timeout=1, keep=False)
        ci.STOP.clear()
        self.uid = patch.object(mac.os, 'getuid', return_value=501, create=True)
        self.uid.start()

    def tearDown(self):
        ci.STOP.clear()
        self.uid.stop()
        self.temporary.cleanup()

    def kit(self, platform='macos'):
        dependencies = self.root / 'input'
        dependencies.mkdir()
        (dependencies / 'preload.cmake').write_text('# fixture')
        (dependencies / 'provision.json').write_text(json.dumps({'platform': platform, 'sources': {
            'preload.cmake': {'origin': 'fixture', 'integrity': 'reviewed'}}}))
        archive = self.root / 'kit.tgz'
        ci.pack(argparse.Namespace(dependencies=dependencies, output=archive))
        return archive

    def test_plan_uses_native_compiler_and_complete_ctest_metadata(self):
        configure, build, test = mac.buildCommands(self.lane, 12, '/CI user/work', '/CI user/deps', '/CI/cache')
        for option in self.lane['common_options'] + self.lane['options']:
            self.assertIn(option, configure)
        for option in ('-DCMAKE_CXX_COMPILER=/usr/bin/clang++', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
                       '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DWEBFRONT_BOOTSTRAP_DIR=/CI user/deps/bootstrap'):
            self.assertIn(option, configure)
        self.assertEqual(build[-1], '4')
        self.assertIn('--no-tests=error', test)
        self.assertIn('--output-junit', test)
        self.assertEqual(test[test.index('-C') + 1], 'Release')

    def test_launchagent_runs_in_aqua_without_privilege_or_persistent_install(self):
        self.args.deadline = 123.25
        with patch.object(mac, 'checkConsole'), patch.object(mac, 'command') as run:
            mac.startTask(self.root, self.args)
        config = plistlib.loads((self.root / (self.args.lane + '.plist')).read_bytes())
        self.assertEqual(config['LimitLoadToSessionType'], 'Aqua')
        self.assertFalse(config['AbandonProcessGroup'])
        self.assertFalse(config['KeepAlive'])
        self.assertNotIn('UserName', config)
        self.assertEqual(config['ProgramArguments'][:2], ['/usr/bin/sandbox-exec', '-f'])
        self.assertEqual(config['ProgramArguments'][-2:], ['--deadline', '123.25'])
        self.assertEqual(set(config['EnvironmentVariables']), {'PATH', 'HOME', 'LANG'})
        self.assertEqual(run.call_args.args[0][1:3], ['bootstrap', mac.guiDomain()])
        self.assertEqual(Path(run.call_args.args[0][-1]).parent, self.root)

    def test_kit_integrity_and_platform(self):
        archive = self.kit()
        with patch.object(mac, 'command') as run:
            mac.prepare(self.root, ci.digest(archive))
        self.assertEqual(run.call_args.args[0][:3], ['git', 'clone', '--mirror'])
        self.assertEqual(run.call_args.kwargs['timeout'], mac.PREPARE_CLONE_TIMEOUT_SECONDS)
        (self.root / 'dependencies/preload.cmake').write_text('tampered')
        with self.assertRaises(ValueError):
            ci.verifyKit(self.root / 'dependencies')

    def test_wrong_platform_is_rejected(self):
        archive = self.kit('windows')
        with patch.object(mac, 'command') as run:
            with self.assertRaisesRegex(ValueError, 'platform=macos'):
                mac.prepare(self.root, ci.digest(archive))
            run.assert_not_called()

    def test_bad_transfer_rejected_before_extraction(self):
        (self.root / 'kit.tgz').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            mac.prepare(self.root, 'bad')
        self.assertFalse((self.root / 'dependencies').exists())

    def test_console_requires_own_uid_and_gui_domain(self):
        with patch.object(mac.os, 'getuid', return_value=501), \
             patch.object(mac.Path, 'stat', return_value=argparse.Namespace(st_uid=502)):
            with self.assertRaises(mac.NoDesktop):
                mac.checkConsole()
        with patch.object(mac.os, 'getuid', return_value=501), \
             patch.object(mac.Path, 'stat', return_value=argparse.Namespace(st_uid=501)), \
             patch.object(mac.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaises(mac.NoDesktop):
                mac.checkConsole()

    def test_locked_disconnected_and_incomplete_sessions_are_skipped(self):
        valid = {'kCGSSessionUserIDKey': os.getuid(), 'kCGSSessionOnConsoleKey': True,
                 'kCGSessionLoginDoneKey': True, 'kCGSSessionAuditIDKey': 123}
        for change in ({'CGSSessionScreenIsLocked': True}, {'kCGSSessionOnConsoleKey': False},
                       {'kCGSessionLoginDoneKey': False}, {'kCGSSessionUserIDKey': -1}):
            with patch.object(mac, 'checkConsole'), patch.object(mac, 'sessionInfo', return_value=valid | change), \
                 patch.object(mac, 'consoleUsers', return_value=[valid | {'CGSSessionScreenIsLocked': False}]):
                with self.assertRaises(mac.NoDesktop):
                    mac.checkDesktop()
        with patch.object(mac, 'checkConsole'), patch.object(mac, 'sessionInfo', return_value=valid), \
             patch.object(mac, 'consoleUsers', return_value=[valid | {'CGSSessionScreenIsLocked': False}]):
            mac.checkDesktop()
        # IORegistry may omit the lock key for an unlocked session.
        with patch.object(mac, 'checkConsole'), patch.object(mac, 'sessionInfo', return_value=valid), \
             patch.object(mac, 'consoleUsers', return_value=[valid]):
            mac.checkDesktop()
        for users in ([], [valid | {'CGSSessionScreenIsLocked': True}],
                      [valid | {'CGSSessionScreenIsLocked': False, 'kCGSSessionAuditIDKey': 999}]):
            with patch.object(mac, 'checkConsole'), patch.object(mac, 'sessionInfo', return_value=valid), \
                 patch.object(mac, 'consoleUsers', return_value=users):
                with self.assertRaises(mac.NoDesktop):
                    mac.checkDesktop()

    def test_missing_desktop_returns_125_and_never_builds(self):
        with patch.object(mac, 'checkDesktop', side_effect=mac.NoDesktop('locked')), \
             patch.object(mac, 'reportToolchain') as report:
            self.assertEqual(mac.session(self.root, self.args, self.lane), 125)
            report.assert_not_called()
        self.assertEqual(json.loads((self.root / (self.args.lane + '.result.json')).read_text())['code'], 125)

    def test_network_precondition_fails_closed(self):
        with patch.object(mac.socket, 'socket') as factory:
            probe = factory.return_value.__enter__.return_value
            probe.connect_ex.return_value = errno.EPERM
            mac.checkNetwork()
            self.assertEqual(probe.connect_ex.call_count, 2)
            for code in (0, errno.ETIMEDOUT, errno.ENETUNREACH):
                probe.connect_ex.return_value = code
                with self.assertRaisesRegex(ValueError, 'network restriction'):
                    mac.checkNetwork()

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS inherited network policy')
    def test_real_policy_blocks_ipv4_ipv6_and_allows_loopback_and_child(self):
        script = '''import socket,errno,subprocess,sys
for family,address in ((socket.AF_INET,('192.0.2.1',443)),(socket.AF_INET6,('2001:db8::1',443))):
    with socket.socket(family,socket.SOCK_STREAM) as s:
        s.settimeout(.2)
        assert s.connect_ex(address) in (errno.EPERM,errno.EACCES)
with socket.socket() as server:
    server.bind(('127.0.0.1',0));server.listen()
    with socket.create_connection(server.getsockname()):
        child,_=server.accept();child.close()
if len(sys.argv)==1:
    subprocess.run([sys.executable,'-c',__import__('os').environ['WF_PROBE'],'child'],check=True)
'''
        result = subprocess.run(['/usr/bin/sandbox-exec', '-p', mac.NETWORK_POLICY, sys.executable, '-c', script],
                                env=dict(os.environ, WF_PROBE=script), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_selected_xcode_and_versions_are_reported(self):
        with patch.object(mac, 'command', return_value=subprocess.CompletedProcess([], 0,
                          'cmake version ' + self.lane['cmake_version'])) as run, \
             patch('sys.stdout', new_callable=io.StringIO) as output:
            mac.reportToolchain(self.lane, io.StringIO())
        invocations = [call.args[0] for call in run.call_args_list]
        self.assertIn(['/usr/bin/xcodebuild', '-version'], invocations)
        self.assertIn(['/usr/bin/xcode-select', '-p'], invocations)
        self.assertIn('No automatic parity', output.getvalue())
        with patch.object(mac, 'command', return_value=subprocess.CompletedProcess([], 0, 'cmake version 1')):
            with self.assertRaisesRegex(ValueError, 'CMake version'):
                mac.reportToolchain(self.lane, io.StringIO())

    def test_session_exports_offline_cache_and_preserves_nonzero_test(self):
        def execute(invocation, log, timeout):
            self.assertEqual(os.environ['CPM_SOURCE_CACHE'], str(self.root.parent.parent / 'cache/cpm'))
            raise subprocess.CalledProcessError(7, invocation)
        with patch.object(mac, 'checkDesktop'), patch.object(mac, 'checkNetwork'), \
             patch.object(mac, 'reportToolchain'), patch.object(mac, 'command'), \
             patch.object(mac, 'runProcess', side_effect=execute), patch.dict(os.environ):
            self.assertEqual(mac.session(self.root, self.args, self.lane), 1)
        self.assertIn('returned non-zero exit status 7', (self.root / (self.args.lane + '.session.log')).read_text())

    def test_failed_lane_preserves_artifacts_and_error_when_stop_fails(self):
        workspace = self.root / self.args.lane
        workspace.mkdir()
        (workspace / 'test-results.xml').write_text('<testsuite failures="1"/>')
        (self.root / (self.args.lane + '.launch.log')).write_text('launch fixture')
        (self.root / (self.args.lane + '.result.json')).write_text('{"code":7}')
        with patch.object(mac, 'startTask'), patch.object(mac, 'stopTask', side_effect=ValueError('stop failed')), \
             patch('sys.stdout', new_callable=io.StringIO), patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(subprocess.CalledProcessError) as error:
                mac.runLane(self.root, self.args)
            self.assertEqual(error.exception.returncode, 7)
        self.assertTrue(workspace.exists())
        with tarfile.open(self.root / (self.args.lane + '-artifacts.tar')) as archive:
            self.assertIn('test-results.xml', archive.getnames())
            self.assertIn(self.args.lane + '.launch.log', archive.getnames())

    def test_success_cleanup_and_keep(self):
        for keep in (True, False):
            workspace = self.root / self.args.lane
            workspace.mkdir(exist_ok=True)
            (self.root / (self.args.lane + '.result.json')).write_text('{"code":0}')
            self.args.keep = keep
            with patch.object(mac, 'startTask'), patch.object(mac, 'stopTask'):
                self.assertEqual(mac.runLane(self.root, self.args), 0)
            self.assertEqual(workspace.exists(), keep)

    def test_no_graphical_domain_is_skipped_without_launch(self):
        with patch.object(mac, 'startTask', side_effect=mac.NoDesktop('no login')), \
             patch.object(mac, 'stopTask'), patch('sys.stdout', new_callable=io.StringIO):
            self.assertEqual(mac.runLane(self.root, self.args), 125)

    def test_timeout_and_cancellation_are_not_success(self):
        with patch.object(mac, 'startTask'), patch.object(mac, 'stopTask'), \
             patch.object(mac, 'SESSION_FINISH_GRACE_SECONDS', .01):
            ci.STOP.set()
            with self.assertRaises(InterruptedError):
                mac.runLane(self.root, self.args)
            ci.STOP.clear()
            self.args.timeout = .01
            with self.assertRaises(TimeoutError):
                mac.runLane(self.root, self.args)

    def test_session_uses_inherited_deadline_and_publishes_complete_timeout_log(self):
        self.args.timeout = 30
        self.args.deadline = time.monotonic() + .05
        def expire(*_):
            time.sleep(max(0, self.args.deadline - time.monotonic()) + .01)
            print('final timeout diagnostic', flush=True)
            raise TimeoutError('shared deadline')
        with patch.object(mac, 'checkDesktop'), patch.object(mac, 'checkNetwork'), \
             patch.object(mac, 'reportToolchain'), patch.object(mac, 'command'), \
             patch.object(mac, 'runProcess', side_effect=expire), patch.dict(os.environ):
            self.assertEqual(mac.session(self.root, self.args, self.lane), 124)
        result = self.root / (self.args.lane + '.result.json')
        self.assertEqual(json.loads(result.read_text())['code'], 124)
        self.assertIn('final timeout diagnostic', (self.root / (self.args.lane + '.session.log')).read_text())

    def test_lane_allows_session_to_finalize_after_shared_deadline(self):
        self.args.timeout = .02
        def launch(root, args):
            def finish():
                time.sleep(max(0, args.deadline - time.monotonic()) + .03)
                (root / (args.lane + '.session.log')).write_text('complete timeout diagnostic')
                (root / (args.lane + '.result.json')).write_text('{"code":124}')
            writer = threading.Thread(target=finish)
            writer.start()
            writers.append(writer)
        writers = []
        try:
            with patch.object(mac, 'startTask', side_effect=launch), patch.object(mac, 'stopTask'), \
                 patch.object(mac, 'SESSION_FINISH_GRACE_SECONDS', .3), \
                 patch('sys.stdout', new_callable=io.StringIO) as output:
                with self.assertRaisesRegex(TimeoutError, 'session deadline'):
                    mac.runLane(self.root, self.args)
                self.assertIn('complete timeout diagnostic', output.getvalue())
            with tarfile.open(self.root / (self.args.lane + '-artifacts.tar')) as archive:
                self.assertEqual(archive.extractfile(self.args.lane + '.session.log').read(),
                                 b'complete timeout diagnostic')
        finally:
            for writer in writers:
                writer.join(timeout=2)

    def test_slow_start_warns_without_failing_before_lane_deadline(self):
        self.args.timeout = 1
        writers = []
        def launch(root, args):
            def finish():
                time.sleep(.15)
                (root / (args.lane + '.session.log')).write_text('slow successful startup')
                (root / (args.lane + '.result.json')).write_text('{"code":0}')
            writer = threading.Thread(target=finish)
            writer.start()
            writers.append(writer)
        try:
            with patch.object(mac, 'startTask', side_effect=launch), patch.object(mac, 'stopTask'), \
                 patch.object(mac, 'STARTUP_WARNING_SECONDS', .01), \
                 patch('sys.stdout', new_callable=io.StringIO) as output:
                self.assertEqual(mac.runLane(self.root, self.args), 0)
                self.assertIn('waiting until the shared deadline', output.getvalue())
        finally:
            for writer in writers:
                writer.join(timeout=2)

    def test_corefoundation_serialization_error_is_reported_and_released(self):
        cg, cf = MagicMock(), MagicMock()
        cg.CGSessionCopyCurrentDictionary.return_value = 1
        cf.CFErrorGetCode.return_value = -99
        def serialize(*arguments):
            ctypes.cast(arguments[-1], ctypes.POINTER(ctypes.c_void_p)).contents.value = 3
            return None
        cf.CFPropertyListCreateData.side_effect = serialize
        with patch.object(mac.ctypes, 'CDLL', side_effect=[cg, cf]):
            with self.assertRaisesRegex(mac.NoDesktop, 'CoreFoundation error -99'):
                mac.sessionInfo()
        self.assertEqual(cf.CFRelease.call_count, 2)
        self.assertEqual(cf.CFRelease.call_args_list[-1].args[0], 1)
        self.assertEqual(cf.CFRelease.call_args_list[0].args[0].value, 3)

    def test_ioreg_supports_dictionary_and_array_roots(self):
        users = [{'kCGSSessionUserIDKey': 501}]
        for root in ({'IOConsoleUsers': users}, [{'IOConsoleUsers': users}]):
            with patch.object(mac, 'command', return_value=subprocess.CompletedProcess([], 0, plistlib.dumps(root))):
                self.assertEqual(mac.consoleUsers(), users)

    def test_cleanup_retains_run_if_stop_or_collection_fails(self):
        self.args.action = 'cleanup'
        self.args.lane = None
        with patch.object(mac, 'stopTask', side_effect=ValueError('unconfirmed')), \
             patch.object(mac, 'cleanTree') as clean, patch('sys.stdout', new_callable=io.StringIO):
            with self.assertRaises(ValueError):
                mac.dispatch(self.args, self.matrix, self.root)
            clean.assert_not_called()
        self.assertTrue(self.root.exists())

    def test_stop_is_idempotent_only_for_confirmed_missing_service(self):
        (self.root / (self.args.lane + '.plist')).touch()
        for diagnosis in ('Could not find service "fixture" in domain', 'permission denied'):
            with patch.object(mac.subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 3, '', 'bootout failed'),
                              subprocess.CompletedProcess([], 113, '', diagnosis)]):
                if diagnosis.startswith('Could not find service'):
                    mac.stopTask(self.root, self.args.lane)
                else:
                    with self.assertRaisesRegex(ValueError, 'not confirmed'):
                        mac.stopTask(self.root, self.args.lane)

    def test_dispatch_rejects_other_machine_lanes(self):
        self.args.action = 'lane'
        self.args.lane = 'desktop-windows-msvc-webview2'
        with self.assertRaisesRegex(ValueError, 'only Mac'):
            mac.dispatch(self.args, self.matrix, self.root)

    def test_macos_cli_requires_kit_hash_pair(self):
        with patch('sys.stderr', new_callable=io.StringIO) as output:
            self.assertEqual(ci.main(['run', '--macos-kit', 'missing.tgz']), 1)
        self.assertIn('supplied together', output.getvalue())

    def test_full_macos_transport_with_native_session_standin(self):
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
        (dependencies / "provision.json").write_text(json.dumps({"platform": "macos", "sources": {
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
    import macos_worker as w
    w.checkNetwork=lambda: None
    w.os.getuid=lambda: 501
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
        def remote(machine, arguments, **kwargs):
            self.assertEqual(machine, "Mac")
            return [sys.executable, str(shim), str(remoteRoot), *arguments]
        args = argparse.Namespace(allow_dirty=False, commit="HEAD", machines="Mac", lanes=None,
                                  kit=None, kit_sha256=None, macos_kit=archive, macos_kit_sha256=ci.digest(archive),
                                  jobs=4, timeout=20, transfer_timeout=20, parallel_lanes=1, keep=False, require_all=True)
        previous = Path.cwd()
        try:
            os.chdir(repository)
            for code in (0, 7, 125):
                with patch.object(ci, "remote", side_effect=remote), \
                     patch.object(ci, "ssh", return_value=[sys.executable, "-c", "print('reachable')"]), \
                     patch.dict(os.environ, WF_TEST_CODE=str(code)), patch("sys.stdout", new_callable=io.StringIO):
                    actual = ci.execute(args)
                    logs = "\n".join(path.read_text(errors="replace") for path in repository.glob("build-local-ci/*/*/*.log"))
                    self.assertEqual(actual, int(bool(code)), logs)
            results = [json.loads(path.read_text()) for path in repository.glob("build-local-ci/*/*/results.json")]
            self.assertEqual({row["results"][0]["status"] for row in results}, {"passed", "failed", "skipped (no interactive session)"})
            for artifact in repository.glob("build-local-ci/*/*/*-artifacts.tar"):
                with tarfile.open(artifact) as stream:
                    self.assertIn("test-results.xml", stream.getnames())
            self.assertEqual(list((remoteRoot / ".webfront-local-ci/runs").iterdir()), [])
        finally:
            os.chdir(previous)


if __name__ == '__main__':
    unittest.main()
