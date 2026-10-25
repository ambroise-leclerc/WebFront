"""Native AppleClang/WKWebView lane in the dedicated account's Aqua session."""
import contextlib
import ctypes
import errno
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import socket
import subprocess
import sys
import threading
import time

from orchestrator import command, digest, extract, verifyKit, runProcess, STOP, validateMatrix
from worker import ctestCommand
from native_worker import collect, cleanTree, runLock

# Applied to the worker before any tested code (including CMake) executes.
# System XPC services are outside this inherited process policy; see LOCAL_CI.md.
NETWORK_POLICY = '''(version 1)
(allow default)
(deny network*)
(allow network-bind (local ip "localhost:*"))
(allow network-inbound (local ip "localhost:*"))
(allow network-outbound (remote ip "localhost:*"))
(allow network-bind (local unix-socket))
(allow network-outbound (remote unix-socket))
'''

STARTUP_WARNING_SECONDS = 15
SESSION_FINISH_GRACE_SECONDS = 5
PREPARE_CLONE_TIMEOUT_SECONDS = 300


class NoDesktop(ValueError):
    pass


def guiDomain():
    return f"gui/{os.getuid()}"


def commandBudget(deadline, seconds):
    return seconds if deadline is None else min(seconds, max(0.1, deadline - time.monotonic()))


def checkConsole(deadline=None):
    if os.getuid() == 0:
        raise ValueError("Use a dedicated non-administrator macOS account")
    if Path('/dev/console').stat().st_uid != os.getuid():
        raise NoDesktop("Log on graphically with the dedicated macOS SSH account")
    result = subprocess.run(['/bin/launchctl', 'print', guiDomain()], capture_output=True, timeout=commandBudget(deadline, 10))
    if result.returncode:
        raise NoDesktop("The dedicated account has no usable Aqua launchd domain")


def sessionInfo():
    cg = ctypes.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')
    cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    cg.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
    cf.CFPropertyListCreateData.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long,
                                          ctypes.c_ulong, ctypes.POINTER(ctypes.c_void_p)]
    cf.CFPropertyListCreateData.restype = ctypes.c_void_p
    cf.CFDataGetLength.argtypes = [ctypes.c_void_p]
    cf.CFDataGetLength.restype = ctypes.c_long
    cf.CFDataGetBytePtr.argtypes = [ctypes.c_void_p]
    cf.CFDataGetBytePtr.restype = ctypes.c_void_p
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFErrorGetCode.argtypes = [ctypes.c_void_p]
    cf.CFErrorGetCode.restype = ctypes.c_long
    info = cg.CGSessionCopyCurrentDictionary()
    if not info:
        raise NoDesktop("No CoreGraphics login session")
    data = None
    error = ctypes.c_void_p()
    try:
        data = cf.CFPropertyListCreateData(None, info, 100, 0, ctypes.byref(error))  # XML plist
        if error.value:
            raise NoDesktop(f"Cannot serialize CoreGraphics session: CoreFoundation error {cf.CFErrorGetCode(error)}")
        if not data:
            raise NoDesktop("Cannot inspect CoreGraphics login session")
        return plistlib.loads(ctypes.string_at(cf.CFDataGetBytePtr(data), cf.CFDataGetLength(data)))
    finally:
        if data:
            cf.CFRelease(data)
        if error.value:
            cf.CFRelease(error)
        cf.CFRelease(info)


def consoleUsers(deadline=None):
    result = command(['/usr/sbin/ioreg', '-n', 'Root', '-d', '1', '-a'],
                     capture_output=True, timeout=commandBudget(deadline, 10))
    registry = plistlib.loads(result.stdout)
    # Older ioreg versions emit an array containing the registry root.
    if isinstance(registry, list):
        registry = registry[0]
    return registry.get('IOConsoleUsers', [])


def checkDesktop(deadline=None):
    checkConsole(deadline)
    info = sessionInfo()
    users = [user for user in consoleUsers(deadline)
             if user.get('kCGSSessionUserIDKey') == os.getuid()
             and user.get('kCGSSessionAuditIDKey') == info.get('kCGSSessionAuditIDKey')]
    if (len(users) != 1 or users[0].get('CGSSessionScreenIsLocked', False) is not False
            or not users[0].get('kCGSSessionOnConsoleKey')
            or info.get('kCGSSessionUserIDKey') != os.getuid()
            or not info.get('kCGSSessionOnConsoleKey')
            or not info.get('kCGSessionLoginDoneKey')
            or info.get('CGSSessionScreenIsLocked', False)):
        raise NoDesktop("Unlock the dedicated macOS account's active graphical session")


def checkNetwork():
    # Fail closed if sandbox-exec did not apply the inherited outbound restriction.
    for family, address in ((socket.AF_INET, ('192.0.2.1', 443)),
                            (socket.AF_INET6, ('2001:db8::1', 443))):
        with socket.socket(family, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(address) not in {errno.EPERM, errno.EACCES}:
                raise ValueError('macOS process network restriction is absent or ineffective')


def buildCommands(lane, jobs, workspace, dependencies, cache):
    workspace, dependencies, cache = map(PurePosixPath, (workspace, dependencies, cache))
    build = workspace / 'build-local-0'
    configure = ['cmake', '-S', str(workspace), '-B', str(build), '-G', 'Ninja',
                 '-C', str(dependencies / 'preload.cmake'), '-DCMAKE_BUILD_TYPE=' + lane['build_type']]
    configure += lane['common_options'] + lane['options']
    configure += ['-DENABLE_CACHE=OFF', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
                  '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                  '-DCPM_SOURCE_CACHE=' + str(cache / 'cpm'),
                  '-DWEBFRONT_BOOTSTRAP_DIR=' + str(dependencies / 'bootstrap'),
                  '-DWEBFRONT_REGISTRY_DIR=' + str(dependencies / 'registry')]
    buildArgs = ['cmake', '--build', str(build), '--parallel', str(min(jobs, 4))]
    if lane['targets']:
        buildArgs += ['--target'] + lane['targets']
    return [configure, buildArgs, ctestCommand(lane, build) + ['-C', lane['build_type'],
            '--output-junit', str(build / 'test-results.xml')]]


def prepare(root, expected):
    if os.getuid() == 0:
        raise ValueError('Use a dedicated non-administrator macOS account')
    if digest(root / 'kit.tgz') != expected:
        raise ValueError('Transferred macOS kit SHA-256 mismatch')
    dependencies = root / 'dependencies'
    dependencies.mkdir()
    # Default APFS is case insensitive: use the portable Windows archive subset.
    extract(root / 'kit.tgz', dependencies, windows=True)
    manifest = verifyKit(dependencies)
    if manifest.get('platform') != 'macos' or manifest['images']:
        raise ValueError('Mac needs a platform=macos kit without Docker images')
    cache = root.parent.parent / 'cache/cpm/cpm'
    cache.mkdir(parents=True, exist_ok=True)
    for module in (dependencies / 'cpm').glob('CPM_*.cmake'):
        temporary = cache / (module.name + '.' + root.name)
        shutil.copyfile(module, temporary)
        temporary.replace(cache / module.name)
    command(['git', 'clone', '--mirror', str(root / 'commit.bundle'), str(root / 'repo.git')],
            timeout=PREPARE_CLONE_TIMEOUT_SECONDS)


def taskName(root, lane):
    return 'org.webfront.local-ci.' + root.name + '.' + lane


def startTask(root, args):
    checkConsole(getattr(args, 'deadline', None))
    profile = root / (args.lane + '.sb')
    profile.write_text(NETWORK_POLICY)
    # Inherit only explicit tool locations, never SSH tokens/proxies or the owner's keychain settings.
    path = os.pathsep.join(dict.fromkeys([str(Path(sys.executable).parent),
                                        *[str(Path(shutil.which(tool) or tool).absolute().parent)
                                          for tool in ('cmake', 'ninja', 'ccache', 'git')],
                                        '/usr/bin', '/bin', '/usr/sbin', '/sbin']))
    label = taskName(root, args.lane)
    plist = {'Label': label, 'ProgramArguments': ['/usr/bin/sandbox-exec', '-f', str(profile),
             sys.executable, '-u', str(root / 'worker.py'), 'session', '--lane', args.lane,
             '--sha', args.sha, '--jobs', str(min(args.jobs, 4)), '--timeout', str(args.timeout),
             '--deadline', str(getattr(args, 'deadline', time.monotonic() + args.timeout))],
             'WorkingDirectory': str(root), 'RunAtLoad': True, 'KeepAlive': False,
             'LimitLoadToSessionType': 'Aqua', 'AbandonProcessGroup': False, 'ExitTimeOut': 5,
             'EnvironmentVariables': {'PATH': path, 'HOME': str(Path.home()), 'LANG': 'en_US.UTF-8'},
             'StandardOutPath': str(root / (args.lane + '.launch.log')),
             'StandardErrorPath': str(root / (args.lane + '.launch.log'))}
    target = root / (args.lane + '.plist')
    target.write_bytes(plistlib.dumps(plist))
    command(['/bin/launchctl', 'bootstrap', guiDomain(), str(target)],
            timeout=commandBudget(getattr(args, 'deadline', None), 15))


def stopTask(root, lane):
    (root / (lane + '.cancel')).touch()
    if not (root / (lane + '.plist')).exists():
        return
    service = guiDomain() + '/' + taskName(root, lane)
    result = subprocess.run(['/bin/launchctl', 'bootout', '--wait', service], capture_output=True, text=True, timeout=15)
    if result.returncode:
        # Only an explicit not-found diagnosis is idempotent. Permissions/IPC failures retain the run.
        probe = subprocess.run(['/bin/launchctl', 'print', service], capture_output=True, text=True,
                               env=dict(os.environ, LC_ALL='C'), timeout=10)
        if not probe.returncode or 'Could not find service' not in probe.stderr:
            raise ValueError('LaunchAgent stop not confirmed: ' + result.stderr + probe.stderr)


def reportToolchain(lane, log, deadline=None):
    def budget(seconds):
        return commandBudget(deadline, seconds)
    version = command(['cmake', '--version'], capture_output=True, text=True, timeout=budget(10)).stdout
    print(version, flush=True)
    if version.splitlines()[0] != 'cmake version ' + lane['cmake_version']:
        raise ValueError('CMake version does not match the macOS lane')
    for invocation in (['/usr/bin/xcode-select', '-p'], ['/usr/bin/xcodebuild', '-version'],
                       ['/usr/bin/xcrun', '--find', 'clang++'], ['/usr/bin/clang++', '--version'],
                       ['/usr/bin/sw_vers'], ['ccache', '--version']):
        command(invocation, stdout=log, stderr=log, timeout=budget(15))
    print('GitHub selects Xcode latest; compare these versions with the same-commit GitHub run. '
          'No automatic parity with the mutable runner image is claimed.', flush=True)


def session(root, args, lane):
    result = root / (args.lane + '.result.json')
    with open(root / (args.lane + '.session.log'), 'w', encoding='utf-8', buffering=1) as log, \
            contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        deadline = getattr(args, 'deadline', None)
        if deadline is None:
            deadline = time.monotonic() + args.timeout
        done = threading.Event()
        def watch():
            while not done.wait(0.1):
                if time.monotonic() >= deadline or (root / (args.lane + '.cancel')).exists():
                    STOP.set()
                    return
        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()
        code = 1
        try:
            checkDesktop(deadline)
            checkNetwork()
            reportToolchain(lane, log, deadline)
            cache = root.parent.parent / 'cache'
            (cache / 'ccache').mkdir(parents=True, exist_ok=True)
            # Independent CTest configure projects need the offline module too.
            os.environ.update(lane.get('env', {}))
            os.environ.update(CPM_SOURCE_CACHE=str(cache / 'cpm'), CCACHE_DIR=str(cache / 'ccache'),
                              CCACHE_BASEDIR=str(root / args.lane))
            workspace = root / args.lane
            command(['git', 'clone', '--no-hardlinks', str(root / 'repo.git'), str(workspace)],
                    stdout=log, stderr=log, timeout=max(0.1, deadline - time.monotonic()))
            command(['git', '-C', str(workspace), 'checkout', '--detach', args.sha],
                    stdout=log, stderr=log, timeout=max(0.1, deadline - time.monotonic()))
            for invocation in buildCommands(lane, args.jobs, workspace, root / 'dependencies', cache):
                print('+', repr(invocation), flush=True)
                runProcess(invocation, log, max(0.1, deadline - time.monotonic()))
            if time.monotonic() >= deadline:
                raise TimeoutError('macOS session deadline expired')
            code = 0
        except NoDesktop as error:
            code = 125
            print('skipped (no interactive session): ' + str(error), flush=True)
        except (ValueError, OSError, subprocess.SubprocessError, InterruptedError, TimeoutError) as error:
            code = 124 if time.monotonic() >= deadline else 1
            print(f'macOS lane: {error}', flush=True)
        finally:
            done.set()
            watcher.join()
    # Close/flush the log before publishing completion to the SSH coordinator.
    temporary = result.with_suffix('.tmp')
    temporary.write_text(json.dumps({'code': code}), encoding='utf-8')
    temporary.replace(result)
    return code


def finishLane(root, args):
    errors = []
    stopped = False
    for stage, action in [('LaunchAgent stop', lambda: stopTask(root, args.lane)),
                          ('Artifact collection', lambda: collect(root, args.lane)),
                          ('Session log', lambda: printLogs(root, args.lane))]:
        try:
            action()
            if stage == 'LaunchAgent stop':
                stopped = True
        except Exception as error:
            errors.append((stage, error))
    if stopped and not errors and not args.keep and (root / args.lane).exists():
        try:
            cleanTree(root, root / args.lane)
        except Exception as error:
            errors.append(('Checkout cleanup', error))
    return errors


def printLogs(root, lane):
    for suffix in ('.launch.log', '.session.log'):
        path = root / (lane + suffix)
        if path.exists():
            print(path.read_text(encoding='utf-8', errors='replace'), flush=True)


def runLane(root, args):
    started = time.monotonic()
    args.deadline = started + args.timeout
    startupWarningAt = started + min(STARTUP_WARNING_SECONDS, args.timeout)
    warned = False
    try:
        with runLock(root):
            if not root.exists() or (root / (args.lane + '.cancel')).exists():
                raise InterruptedError('macOS run already stopped or removed')
            if time.monotonic() >= args.deadline:
                raise TimeoutError('macOS lane deadline expired before launch')
            try:
                startTask(root, args)
            except subprocess.TimeoutExpired as error:
                if time.monotonic() >= args.deadline:
                    raise TimeoutError('macOS launch deadline expired') from error
                raise
            except NoDesktop as error:
                print('skipped (no interactive session): ' + str(error), flush=True)
                return 125
        result = root / (args.lane + '.result.json')
        while not result.exists():
            if STOP.wait(0.1) or not root.exists() or (root / (args.lane + '.cancel')).exists():
                raise InterruptedError('macOS lane interrupted')
            now = time.monotonic()
            if now >= args.deadline + SESSION_FINISH_GRACE_SECONDS:
                raise TimeoutError('macOS lane deadline expired; inspect the launch and session logs')
            if not warned and now >= startupWarningAt and not (root / (args.lane + '.session.log')).exists():
                print('LaunchAgent has not written its session log yet; waiting until the shared deadline', flush=True)
                warned = True
        code = json.loads(result.read_text(encoding='utf-8'))['code']
        if code == 125:
            return code
        if code == 124:
            raise TimeoutError('macOS session deadline expired')
        if code:
            raise subprocess.CalledProcessError(code, 'macOS graphical session')
        return 0
    finally:
        original = sys.exc_info()[1]
        try:
            with runLock(root):
                errors = finishLane(root, args) if root.exists() else []
        except Exception as error:
            errors = [('Finalization lock', error)]
        for stage, error in errors:
            print(f'{stage} failed: {error}', file=sys.stderr, flush=True)
        if errors and original is None:
            raise errors[0][1]


def dispatch(args, matrix, root):
    validateMatrix(matrix)
    lane = None
    if args.lane:
        lane = next(row for row in matrix['lanes'] if row['id'] == args.lane)
        if lane['machine'] != 'Mac' or lane['runtime'] != 'native:appleclang':
            raise ValueError('macOS worker accepts only Mac native:appleclang lanes')
    if args.action == 'prepare':
        prepare(root, args.kit_sha256)
    elif args.action == 'lane':
        return runLane(root, args)
    elif args.action == 'session':
        return session(root, args, lane)
    elif args.action == 'stop':
        with runLock(root):
            if root.exists():
                args.keep = True
                errors = finishLane(root, args)
                if errors:
                    raise errors[0][1]
    elif args.action == 'artifacts':
        with runLock(root):
            with open(root / (args.lane + '-artifacts.tar'), 'rb') as source:
                shutil.copyfileobj(source, sys.stdout.buffer)
    elif args.action == 'cleanup':
        with runLock(root):
            if root.exists():
                for row in matrix['lanes']:
                    if row['machine'] == 'Mac':
                        args.lane = row['id']
                        args.keep = True
                        errors = finishLane(root, args)
                        if errors:
                            raise errors[0][1]
                cleanTree(root, root)
    else:
        raise ValueError('Unsupported macOS action')
    return 0
