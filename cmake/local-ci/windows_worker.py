"""Native Windows lane, launched in the dedicated account's interactive session.

Provisioning (including host firewall policy) is always separate from execution.
"""
import base64
import contextlib
import ctypes
from ctypes import wintypes
import ipaddress
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import threading
import time

from orchestrator import command, digest, extract, verifyKit, psQuote, STOP, runProcess
from worker import ctestCommand


def ps(script, **kwargs):
    encoded = base64.b64encode(("$ErrorActionPreference='Stop'; " + script).encode("utf-16-le")).decode()
    try:
        return command(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
                        "-EncodedCommand", encoded], **kwargs)
    except subprocess.CalledProcessError as error:
        # Probes capture output for parsing; preserve their actual diagnosis in the lane log.
        for output in (error.stdout, error.stderr):
            if output:
                print(output, file=sys.stderr, flush=True)
        raise


def publicNetworks():
    # LAN SSH remains available. This is intentionally weaker than Linux network=none.
    networks = [ipaddress.ip_network("0.0.0.0/0"), ipaddress.ip_network("::/0")]
    for allowed in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8",
                    "169.254.0.0/16", "::1/128", "fc00::/7", "fe80::/10"):
        subnet = ipaddress.ip_network(allowed)
        networks = [part for network in networks for part in
                    (list(network.address_exclude(subnet)) if network.version == subnet.version
                     and subnet.subnet_of(network) else [network])]
    return sorted(str(network) for network in networks)


def firewallSetup():
    addresses = ",".join(psQuote(address) for address in publicNetworks())
    return ("# Dedicated CI host only; run once as administrator, after reviewing LAN access.\n"
            "Set-NetFirewallProfile -Profile Domain,Private,Public -Enabled True\n"
            "New-NetFirewallRule -Name WebFrontLocalCI-BlockPublic -DisplayName 'WebFront CI public egress block' "
            "-Direction Outbound -Action Block -Enabled True -Profile Any -Protocol Any "
            "-RemoteAddress " + addresses + "\n")


def checkNetwork():
    # Inspect effective policy, not a user-provided marker. Explicit block rules override allows.
    result = ps("""
if (@(Get-NetFirewallProfile -PolicyStore ActiveStore | Where-Object { -not $_.Enabled }).Count) {
    throw 'All firewall profiles must be enabled'
}
$rule = Get-NetFirewallRule -PolicyStore ActiveStore -Name WebFrontLocalCI-BlockPublic
if ($rule.Enabled -ne 'True' -or $rule.Direction -ne 'Outbound' -or $rule.Action -ne 'Block' -or
    $rule.Profile -ne 'Any') { throw 'Public egress block rule is missing or ineffective' }
$address = $rule | Get-NetFirewallAddressFilter
$port = $rule | Get-NetFirewallPortFilter
$app = $rule | Get-NetFirewallApplicationFilter
$service = $rule | Get-NetFirewallServiceFilter
$interface = $rule | Get-NetFirewallInterfaceFilter
$interfaceType = $rule | Get-NetFirewallInterfaceTypeFilter
if ($address.LocalAddress -ne 'Any' -or $port.Protocol -ne 'Any' -or
    $app.Program -ne 'Any' -or $app.Package -ne 'Any' -or $service.Service -ne 'Any' -or
    $interface.InterfaceAlias -ne 'Any' -or $interfaceType.InterfaceType -ne 'Any') {
    throw 'Public egress block rule must cover every application, protocol and interface'
}
ConvertTo-Json -Compress -InputObject @($address.RemoteAddress)
""", capture_output=True, text=True, timeout=30)
    actual = json.loads(result.stdout)
    def normalized(addresses):
        networks = []
        for value in addresses:
            if "-" in value:
                first, last = value.split("-", 1)
                networks.extend(ipaddress.summarize_address_range(ipaddress.ip_address(first),
                                                                  ipaddress.ip_address(last)))
            else:
                networks.append(ipaddress.ip_network(value, strict=False))
        return {str(net) for version in (4, 6) for net in
                ipaddress.collapse_addresses(n for n in networks if n.version == version)}
    if normalized(actual) != normalized(publicNetworks()):
        raise ValueError("Public egress block does not cover the required IPv4/IPv6 ranges")


def checkDesktop():
    session = wintypes.DWORD()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    if not kernel.ProcessIdToSessionId(os.getpid(), ctypes.byref(session)) or not session.value:
        raise ValueError("Windows lane needs an interactive session; SSH/session 0 is insufficient")
    wts = ctypes.WinDLL("wtsapi32", use_last_error=True)
    data, size = ctypes.c_void_p(), wintypes.DWORD()
    if not wts.WTSQuerySessionInformationW(None, session.value, 8, ctypes.byref(data), ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if ctypes.cast(data, ctypes.POINTER(wintypes.DWORD)).contents.value != 0:  # WTSActive
            raise ValueError("The Windows graphical session is disconnected")
    finally:
        wts.WTSFreeMemory(data)
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.OpenInputDesktop.restype = wintypes.HANDLE
    desktop = user.OpenInputDesktop(0, False, 0x0100)  # DESKTOP_SWITCHDESKTOP
    if not desktop:
        raise ValueError("The Windows input desktop is unavailable; unlock the dedicated session")
    user.CloseDesktop.argtypes = [wintypes.HANDLE]
    user.CloseDesktop(desktop)


def ownProcessTree():
    """Keep the Job handle alive: killing the scheduled worker also kills all descendants."""
    class BasicLimits(ctypes.Structure):
        _fields_ = [("ProcessTime", ctypes.c_int64), ("JobTime", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinWorkingSet", ctypes.c_size_t),
                    ("MaxWorkingSet", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]
    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in
                    ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes", "OtherBytes")]
    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("Basic", BasicLimits), ("Io", IoCounters),
                    ("ProcessMemory", ctypes.c_size_t), ("JobMemory", ctypes.c_size_t),
                    ("PeakProcessMemory", ctypes.c_size_t), ("PeakJobMemory", ctypes.c_size_t)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    handle = kernel.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if (not handle or not kernel.SetInformationJobObject(handle, 9, ctypes.byref(limits), ctypes.sizeof(limits))
            or not kernel.AssignProcessToJobObject(handle, kernel.GetCurrentProcess())):
        raise ctypes.WinError(ctypes.get_last_error())
    # Never close this handle while the worker is alive (the worker itself belongs to the Job).
    return handle


def msvcEnvironment():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / \
        "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = command([str(vswhere), "-latest", "-products", "*", "-requires",
                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                           capture_output=True, text=True, timeout=30).stdout.strip()
    devcmd = Path(installation) / "Common7/Tools/VsDevCmd.bat"
    if not installation or not devcmd.is_file():
        raise ValueError("Install Visual Studio MSVC x64 C++ tools and a Windows SDK")
    # devcmd comes from vswhere, not from the commit/kit. Only this fixed shell sequence is evaluated.
    # cmd's /s quoting is not CommandLineToArgvW: list2cmdline would escape these
    # inner quotes and break every installation under Program Files.
    result = command('cmd.exe /d /s /c ""' + str(devcmd) +
                     '" -no_logo -arch=x64 -host_arch=x64 >nul && set"',
                     capture_output=True, text=True, timeout=60)
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line and not line.startswith("="))


def checkRuntime():
    result = ps("""
$roots = @("${env:ProgramFiles(x86)}/Microsoft/EdgeWebView/Application",
           "$env:ProgramFiles/Microsoft/EdgeWebView/Application",
           "$env:LOCALAPPDATA/Microsoft/EdgeWebView/Application")
$runtime = @($roots | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Filter msedgewebview2.exe -Recurse
})
if (-not $runtime.Count) { throw 'Provision WebView2 Evergreen before running offline' }
$runtime | ForEach-Object {
    if (-not $_.VersionInfo.FileVersion) { throw 'WebView2 runtime has no version' }
    Write-Output "WebView2 $($_.VersionInfo.FileVersion) at $($_.FullName)"
}
""", capture_output=True, text=True, timeout=30)
    print(result.stdout, flush=True)


def buildCommands(lane, jobs, workspace, dependencies, cache):
    workspace, dependencies, cache = map(PureWindowsPath, (workspace, dependencies, cache))
    build = workspace / "build-local-0"
    configure = ["cmake", "-S", str(workspace), "-B", str(build), "-G", "Ninja",
                 "-C", str(dependencies / "preload.cmake"), "-DCMAKE_BUILD_TYPE=" + lane["build_type"]]
    configure += lane["common_options"] + lane["options"]
    configure += ["-DENABLE_CACHE=OFF", "-DCMAKE_CXX_COMPILER=cl", "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
                  "-DCPM_SOURCE_CACHE=" + cache.as_posix() + "/cpm", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                  "-DWEBFRONT_BOOTSTRAP_DIR=" + (dependencies / "bootstrap").as_posix(),
                  "-DWEBFRONT_REGISTRY_DIR=" + (dependencies / "registry").as_posix()]
    buildArgs = ["cmake", "--build", str(build), "--config", lane["build_type"],
                 "--parallel", str(min(jobs, 4))]
    if lane["targets"]:
        buildArgs += ["--target"] + lane["targets"]
    return [configure, buildArgs, ctestCommand(lane, build) + ["-C", lane["build_type"],
            "--output-junit", str(build / "test-results.xml")]]


def prepare(root, expected):
    if digest(root / "kit.tgz") != expected:
        raise ValueError("Transferred Windows kit SHA-256 mismatch")
    dependencies = root / "dependencies"
    dependencies.mkdir()
    extract(root / "kit.tgz", dependencies)
    manifest = verifyKit(dependencies)
    if manifest.get("platform") != "windows" or manifest["images"]:
        raise ValueError("Windows needs a platform=windows kit without Docker images")
    checkNetwork()
    cache = root.parent.parent / "cache/cpm/cpm"
    cache.mkdir(parents=True, exist_ok=True)
    for module in (dependencies / "cpm").glob("CPM_*.cmake"):
        temporary = cache / (module.name + "." + root.name)
        shutil.copyfile(module, temporary)
        temporary.replace(cache / module.name)
    command(["git", "clone", "--mirror", str(root / "commit.bundle"), str(root / "repo.git")])


def taskName(root, lane):
    return "WebFrontCI-" + root.name + "-" + lane


def startTask(root, args):
    arguments = subprocess.list2cmdline(["-u", str(root / "worker.py"), "session", "--lane", args.lane,
                                         "--sha", args.sha, "--jobs", str(min(args.jobs, 4)),
                                         "--timeout", str(args.timeout)])
    name = psQuote(taskName(root, args.lane))
    ps(f"""
$action = New-ScheduledTaskAction -Execute {psQuote(sys.executable)} -Argument {psQuote(arguments)} -WorkingDirectory {psQuote(root)}
$principal = New-ScheduledTaskPrincipal -UserId ([System.Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds {args.timeout + 30}) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName {name} -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName {name}
""", timeout=30)


def stopTask(root, lane):
    (root / (lane + ".cancel")).touch()
    name = psQuote(taskName(root, lane))
    ps(f"""
$task = Get-ScheduledTask -TaskName {name} -ErrorAction SilentlyContinue
if ($task) {{
    Stop-ScheduledTask -TaskName {name}
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-ScheduledTask -TaskName {name}).State -eq 'Running') {{
        if ((Get-Date) -gt $deadline) {{ throw 'Scheduled task cancellation was not confirmed' }}
        Start-Sleep -Milliseconds 100
    }}
    Unregister-ScheduledTask -TaskName {name} -Confirm:$false
}}
""", timeout=15)


def session(root, args, lane):
    result = root / (args.lane + ".result.json")
    with open(root / (args.lane + ".session.log"), "w", encoding="utf-8", buffering=1) as log, \
            contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        deadline = time.monotonic() + args.timeout
        done = threading.Event()
        def watch():
            while not done.wait(0.1):
                if time.monotonic() >= deadline or (root / (args.lane + ".cancel")).exists():
                    STOP.set()
                    return
        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()
        code = 1
        try:
            job = ownProcessTree()
            checkDesktop()
            checkNetwork()
            os.environ.update(msvcEnvironment())
            os.environ.update(lane.get("env", {}))
            cache = root.parent.parent / "cache"
            (cache / "sccache").mkdir(parents=True, exist_ok=True)
            # CTest's independent CMake projects do not inherit configure's -D entries.
            os.environ["CPM_SOURCE_CACHE"] = (cache / "cpm").as_posix()
            os.environ["SCCACHE_DIR"] = str(cache / "sccache")
            # A lane owns its server so cancel/timeout cannot kill another run's cache server.
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                os.environ["SCCACHE_SERVER_PORT"] = str(listener.getsockname()[1])
            os.environ["SCCACHE_IDLE_TIMEOUT"] = "0"
            version = command(["cmake", "--version"], capture_output=True, text=True).stdout
            print(version, flush=True)
            if version.splitlines()[0] != "cmake version " + lane["cmake_version"]:
                raise ValueError("CMake version does not match the Windows lane")
            command(["cl"], stdout=log, stderr=log)  # compiler banner and effective toolset
            command(["sccache", "--version"], stdout=log, stderr=log)
            checkRuntime()
            workspace = root / args.lane
            command(["git", "clone", "--no-hardlinks", str(root / "repo.git"), str(workspace)], stdout=log, stderr=log)
            command(["git", "-C", str(workspace), "checkout", "--detach", args.sha], stdout=log, stderr=log)
            for invocation in buildCommands(lane, args.jobs, workspace, root / "dependencies", cache):
                print("+", repr(invocation), flush=True)
                runProcess(invocation, log, max(0.1, deadline - time.monotonic()))
            code = 0
        except (ValueError, OSError, subprocess.SubprocessError, InterruptedError, TimeoutError) as error:
            code = 124 if time.monotonic() >= deadline else 1
            print(f"Windows lane: {error}", flush=True)
        finally:
            done.set()
            watcher.join()
            temporary = result.with_suffix(".tmp")
            temporary.write_text(json.dumps({"code": code}), encoding="utf-8")
            temporary.replace(result)
    return code


def collect(root, lane):
    workspace = root / lane
    with tarfile.open(root / (lane + "-artifacts.tar"), "w") as archive:
        log = root / (lane + ".session.log")
        if log.is_file():
            archive.add(log, arcname=log.name)
        for pattern in ("**/test-results.xml", "**/LastTest.log", "**/CMakeConfigureLog.yaml"):
            for path in workspace.glob(pattern):
                if path.is_file() and not path.is_symlink():
                    archive.add(path, arcname=path.relative_to(workspace), recursive=False)


def cleanTree(root, target):
    root, target = root.resolve(), target.resolve()
    if (root.parent.name != "runs" or root.parent.parent.name != ".webfront-local-ci"
            or not target.is_relative_to(root)):
        raise ValueError("Refusing cleanup outside a local CI run")
    def readonly(function, path, error):
        # Git object files are read-only on Windows, even in an owned disposable clone.
        if not Path(path).resolve().is_relative_to(target):
            raise ValueError("Cleanup path escaped the local CI run")
        if not os.stat(path).st_mode & stat.S_IWRITE:
            os.chmod(path, stat.S_IWRITE)
            function(path)
        else:
            raise error[1]
    shutil.rmtree(target, onerror=readonly)


def runLane(root, args):
    try:
        startTask(root, args)
        deadline = time.monotonic() + args.timeout
        result = root / (args.lane + ".result.json")
        while not result.exists():
            if STOP.wait(0.1):
                raise InterruptedError("Windows lane interrupted")
            if time.monotonic() >= deadline:
                raise TimeoutError("Windows lane deadline expired (check the interactive account/session)")
            if (time.monotonic() > deadline - args.timeout + 15
                    and not (root / (args.lane + ".session.log")).exists()):
                raise ValueError("Interactive task did not start; log on and unlock the dedicated Windows account")
        code = json.loads(result.read_text(encoding="utf-8"))["code"]
        if code == 124:
            raise TimeoutError("Windows session deadline expired")
        if code:
            raise subprocess.CalledProcessError(code, "Windows graphical session")
    finally:
        # The task's Job kills descendants even if the SSH execution channel disappears.
        stopTask(root, args.lane)
        collect(root, args.lane)
        log = root / (args.lane + ".session.log")
        if log.exists():
            print(log.read_text(encoding="utf-8", errors="replace"), flush=True)
        if not args.keep and (root / args.lane).exists():
            cleanTree(root, root / args.lane)


def dispatch(args, matrix, root):
    lane = None
    if args.lane:
        lane = next(row for row in matrix["lanes"] if row["id"] == args.lane)
        if lane["runtime"] != "native:msvc":
            raise ValueError("Windows worker accepts only native:msvc lanes")
    if args.action == "prepare":
        prepare(root, args.kit_sha256)
    elif args.action == "lane":
        runLane(root, args)
    elif args.action == "session":
        return session(root, args, lane)
    elif args.action == "stop":
        stopTask(root, args.lane)
    elif args.action == "artifacts":
        # Windows text-mode stdout translates LF bytes; the SSH artifact stream must be binary.
        if os.name == "nt":
            import msvcrt
            msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
        with open(root / (args.lane + "-artifacts.tar"), "rb") as source:
            shutil.copyfileobj(source, sys.stdout.buffer)
    elif args.action == "cleanup":
        if root.parent.name != "runs" or root.parent.parent.name != ".webfront-local-ci":
            raise ValueError("Refusing cleanup outside a local CI run")
        # A failed cancellation must never turn cleanup into deletion of a live task's files.
        for row in matrix["lanes"]:
            if row["runtime"] == "native:msvc":
                stopTask(root, row["id"])
        cleanTree(root, root)
    else:
        raise ValueError("Unsupported Windows action")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--print-firewall-setup"]:
        print(firewallSetup())
    else:
        raise SystemExit("Use worker.py, or --print-firewall-setup during host provisioning")
