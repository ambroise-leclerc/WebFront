"""Real MCP clients (stdio and Streamable HTTP) driving an authorized developer window, with no third-party Python modules."""
import argparse
import http.client
import urllib.parse
import json
import pathlib
import queue
import re
import shutil
import subprocess
import socket
import sys
import os
import base64
import contextlib
import hashlib
import io
import itertools
import tarfile
import tempfile
import threading
import time
import signal


class Client:
    def __init__(self, command, stderr, timeout=30, env=None):
        self.timeout = timeout  # seconds for the session to become ready and for one operation to finish
        self.log_path = pathlib.Path(stderr.name) if isinstance(getattr(stderr, "name", None), str) else None
        self.child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr, text=True, encoding="utf-8", env=env)
        self.messages = queue.Queue()
        self.counter = 0
        def read():
            for line in self.child.stdout:
                try:
                    self.messages.put(json.loads(line))
                except Exception as error:
                    self.messages.put(error)
            self.messages.put(EOFError("MCP stdout closed"))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, method, params=None, notification=False):
        self.counter += 1
        request = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            request["params"] = params
        if not notification:
            request["id"] = self.counter
        self.child.stdin.write(json.dumps(request) + "\n")
        self.child.stdin.flush()
        if notification:
            return None
        message = self.messages.get(timeout=30)
        if isinstance(message, Exception):
            if isinstance(message, EOFError):
                details = self.log_path.read_text()[-2000:] if self.log_path and self.log_path.exists() else ""
                raise EOFError(f"MCP stdout closed (status {self.child.poll()}); native stderr: {details}")
            raise message
        assert message["id"] == self.counter, message
        assert "error" not in message, message
        return message["result"]

    def tool(self, name, arguments=None, error=False):
        result = self.send("tools/call", {"name": "webfront." + name, "arguments": arguments or {}})
        assert result.get("isError", False) == error, result
        assert json.loads(result["content"][0]["text"]) == result["structuredContent"]
        return result["structuredContent"]

    def initialize(self):
        result = self.send("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                          "clientInfo": {"name": "WebFront-regression", "version": "1"}})
        assert result["protocolVersion"] == "2025-11-25", result
        self.send("notifications/initialized", notification=True)
        assert len(self.send("tools/list")["tools"]) == 4
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            state = self.tool("session")
            if state["state"] == "ready":
                self.session = state["session"]
                return
            assert state["state"] != "closed", state
            time.sleep(0.05)
        raise TimeoutError("Developer session did not become ready")

    def start(self, operation, arguments=None):
        return self.tool("start", {"session": self.session, "operation": operation,
                                   "arguments": arguments or {}})["runId"]

    def result(self, run):
        return self.tool("result", {"session": self.session, "runId": run})

    def wait(self, run):
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            result = self.result(run)
            if result["state"] not in ("queued", "running"):
                return result
            time.sleep(0.05)
        raise TimeoutError("Operation did not finish")

    def run(self, operation, arguments=None, state="succeeded"):
        result = self.wait(self.start(operation, arguments))
        assert result["state"] == state, result
        return result["result"]

    def close(self):
        if self.child.poll() is None:
            self.child.stdin.close()
            # The agent ends on its own within about 9 s when a browser does not close: 5 s of grace, then up to 2 s to
            # end its processes and 2 s to remove its profile. Wait longer than that, so a loaded machine is not a failure.
            try:
                self.child.wait(timeout=30)
            except subprocess.TimeoutExpired as error:
                self.child.kill()
                self.child.wait()
                details = self.log_path.read_text()[-2000:] if self.log_path and self.log_path.exists() else ""
                raise TimeoutError(f"The agent did not exit after its input closed; native stderr: {details}") from error
        self.reader.join(timeout=2)


class HttpClient(Client):
    """The same MCP calls over the Streamable HTTP listener of an application (ADR-025), from its private mcp.json."""

    def __init__(self, configuration, timeout=30):
        self.timeout = timeout
        server = json.loads(pathlib.Path(configuration).read_text())["mcpServers"]["webfront"]
        assert server["type"] == "http", server
        self.url = urllib.parse.urlsplit(server["url"])
        assert self.url.hostname == "127.0.0.1" and self.url.path == "/mcp", server["url"]
        self.authorization = server["headers"]["Authorization"]
        self.mcp_session = None
        self.counter = 0

    def post(self, body, headers=None, method="POST"):
        """One request on its own connection (the listener closes each one); returns (status, headers, body)."""
        connection = http.client.HTTPConnection(self.url.hostname, self.url.port, timeout=30)
        try:
            sent = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                    "Authorization": self.authorization}
            if self.mcp_session:
                sent["Mcp-Session-Id"] = self.mcp_session
                sent["MCP-Protocol-Version"] = "2025-11-25"
            sent.update(headers or {})
            for name in [name for name, value in sent.items() if value is None]:
                del sent[name]
            connection.request(method, self.url.path, body=body, headers=sent)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def send(self, method, params=None, notification=False):
        self.counter += 1
        request = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            request["params"] = params
        if not notification:
            request["id"] = self.counter
        status, headers, body = self.post(json.dumps(request).encode())
        if notification:
            assert status == 202 and not body, (status, body)
            return None
        assert status == 200, (status, body)
        assert headers.get("Content-Type") == "application/json", headers
        if method == "initialize":
            self.mcp_session = headers["Mcp-Session-Id"]
        message = json.loads(body)
        assert message["id"] == self.counter, message
        assert "error" not in message, message
        return message["result"]

    def close(self):
        if self.mcp_session:
            status, _, _ = self.post(None, {"Content-Type": None, "Accept": None}, method="DELETE")
            assert status == 204, status
            self.mcp_session = None


@contextlib.contextmanager
def profile_directory():
    """A private directory in the system temporary directory, whose removal tolerates what the agent still removes.

    tempfile.TemporaryDirectory takes ignore_cleanup_errors only from Python 3.10. On Linux, the directory is in /tmp
    rather than an inherited TMPDIR, which may be long: Chromium's singleton socket path must stay below sockaddr_un's
    limit."""
    root = "/tmp" if sys.platform.startswith("linux") and os.access("/tmp", os.W_OK | os.X_OK) else None
    path = tempfile.mkdtemp(prefix="wf-profile- &%!^ 雪 " if sys.platform == "win32" else "wf-profile-", dir=root)
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def remaining_profiles(profiles, timeout=25, settle=2.5):
    """The agent profiles left in `profiles`, once absent for `settle` seconds or after `timeout` more, with what
    they contain.

    On Linux, the CEF browser runs inside the agent and may write until it exits: the agent then leaves the
    last removal to a process that waits for its exit and repeats it for 2 s against a helper relaunched meanwhile.
    A profile is gone only once absent for longer than that."""
    deadline = time.monotonic() + timeout + settle
    absent_since = None
    while True:
        left = list(profiles.glob("webfront-agent-*"))
        now = time.monotonic()
        if left:
            absent_since = None
        elif absent_since is None:
            absent_since = now
        if (absent_since is not None and now - absent_since >= settle) or now >= deadline:
            # os.walk skips a directory removed while it is listed: the removal may still be under way. Only the
            # first 40 entries are walked.
            contents = (str(pathlib.Path(directory, name).relative_to(profiles))
                        for profile in left for directory, folders, files in os.walk(profile) for name in folders + files)
            return left, list(itertools.islice(contents, 40))
        time.sleep(0.05)


def check_windows_profile_cleaner(agent, root):
    """A locked file must be retried, including when its Unicode path contains shell metacharacters."""
    if sys.platform != "win32":
        return
    import ctypes
    from ctypes import wintypes
    profile = root / "profile &%!^ 雪"
    profile.mkdir()
    file = profile / "locked"
    file.write_text("locked")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(file), 0x80000000, 0, None, 3, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        cleaner = subprocess.Popen([agent, "--cleanup-profile", str(profile)])
        time.sleep(3)
        assert cleaner.poll() is None, "Cleaner stopped while the profile was locked"
    finally:
        kernel.CloseHandle(handle)
    assert cleaner.wait(timeout=10) == 0
    assert not profile.exists(), "Cleaner left the profile behind"


def check_startup_backoff(agent, root, ready_delay=0):
    """The attach-only path needs no renderer and must not create 50 connections per second."""
    endpoint = root / f"owner-{ready_delay}"
    endpoint.mkdir(mode=0o700)
    stopped = threading.Event()
    calls = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.1)
        (endpoint / "endpoint.json").write_text(json.dumps({"version": 1, "port": listener.getsockname()[1], "token": "f" * 64}))
        (endpoint / "endpoint.json").chmod(0o600)
        def serve():
            while not stopped.is_set():
                try:
                    peer, _ = listener.accept()
                except socket.timeout:
                    continue
                with peer:
                    peer.settimeout(2)
                    message = b""
                    while b"\n" not in message:
                        chunk = peer.recv(4096)
                        if not chunk:
                            break
                        message += chunk
                    if b"\n" not in message:
                        continue
                    request = json.loads(message)
                    calls.append(request["method"])
                    if ready_delay and stopped.wait(ready_delay):
                        continue
                    state = "ready" if ready_delay else "starting"
                    try:
                        peer.sendall(json.dumps({"result": {"session": "starting", "state": state}}).encode() + b"\n")
                    except OSError:
                        pass  # A correctly bounded client already closed its socket.
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            began = time.monotonic()
            result = subprocess.run([agent, "--attach", str(endpoint), "--operation", "build", "--startup-timeout", "1"],
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 71, result.stdout + result.stderr
            assert 1 <= len(calls) <= 6 and set(calls) == {"describe"}, calls
            if ready_delay:
                assert calls == ["describe"], calls
            if ready_delay >= 2:
                assert time.monotonic() - began < 1.8, "Transport exceeded the startup deadline"
        finally:
            stopped.set()
            worker.join(timeout=3)
            assert not worker.is_alive()


def check_result_backoff(agent, root):
    """One second of work should not need fifty new TCP connections."""
    endpoint = root / "result-backoff"
    endpoint.mkdir(mode=0o700)
    stopped = threading.Event()
    polls = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.1)
        (endpoint / "endpoint.json").write_text(json.dumps({"version": 1, "port": listener.getsockname()[1], "token": "f" * 64}))
        (endpoint / "endpoint.json").chmod(0o600)
        began = time.monotonic()
        def serve():
            while not stopped.is_set():
                try:
                    peer, _ = listener.accept()
                except socket.timeout:
                    continue
                with peer:
                    peer.settimeout(2)
                    message = b""
                    while b"\n" not in message:
                        message += peer.recv(4096)
                    method = json.loads(message)["method"]
                    result = {"session": "owner", "state": "ready"}
                    if method == "start":
                        result = {"runId": "run-1"}
                    elif method == "result":
                        polls.append(time.monotonic())
                        result = {"state": "running"} if time.monotonic() - began < 1 else {"state": "succeeded", "result": {"exitCode": 0, "output": "done"}}
                    peer.sendall(json.dumps({"result": result}).encode() + b"\n")
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            result = subprocess.run([agent, "--attach", str(endpoint), "--operation", "build"], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, result.stdout + result.stderr
            assert 2 <= len(polls) <= 6, polls
        finally:
            stopped.set()
            worker.join(timeout=3)
            assert not worker.is_alive()


def watchdog(args):
    """Exercise native revocation with a real blocked renderer, without compiling Jasmine first.

    Only the test archive replaces the operation body. The real signed callback, runner, native
    deadline, window close grace, stdio EOF and endpoint cleanup remain on the production path.
    The agent is driven through MCP, the way agents drive it; the CLI shares the same native
    revocation, and its own exit statuses are checked by the failed relaunch below.
    """
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="watchdog-") as temporary:
        root = pathlib.Path(temporary)
        check_windows_profile_cleaner(args.agent, root)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        environment = root / "watchdog-environment.tgz"
        blocked = b""" = async (_argv,context) => {
            const call=(name,...args) => context.bridge.cppFunction('webfront.developer.'+name)(...args);
            const id=await call('begin','renderer.started',false);
            await call('write',id,'output',0,new Uint8Array([1]));
            await call('commit',id);
            while(true) {}
        };"""
        replaced = False
        with tarfile.open(args.environment, "r:gz") as archive, tarfile.open(environment, "w:gz", compresslevel=1) as output:
            for member in archive.getmembers():
                data = archive.extractfile(member).read() if member.isfile() else None
                if member.name == "package/runtime.mjs":
                    # Locate the named export, rather than hard-coding a minifier's local identifier.
                    exports = re.findall(rb"([A-Za-z_$][A-Za-z0-9_$]*) as runNonInteractive(?=[,}])", data)
                    assert len(exports) == 1, "The watchdog fixture needs the real runNonInteractive export"
                    data += b"\n" + exports[0] + blocked
                    connect = re.findall(rb"([A-Za-z_$][A-Za-z0-9_$]*) as connectAutomation(?=[,}])", data)
                    assert len(connect) == 1
                    # A bridge RangeError (a message past the bridge's maximum, lowered for this call) consumes a JS
                    # sequence without reaching native code.
                    data += (b"\nconst beforeGap=" + connect[0] + b";" + connect[0] +
                             b"=context=>{const limit=context.bridge.maxMessageBytes;context.bridge.maxMessageBytes=70000;"
                             b"void context.call('poll','x'.repeat(70000)).then(()=>window.close(),"
                             b"()=>{context.bridge.maxMessageBytes=limit;beforeGap(context);});};")
                    # Only this startup test needs a delay longer than the operation's two-second deadline.
                    data = b"await new Promise(resolve => setTimeout(resolve,3000));\n" + data
                    member.size = len(data)
                    replaced = True
                output.addfile(member, io.BytesIO(data) if data is not None else None)
        assert replaced
        command = ["--project", str(project), "--environment", str(environment),
                   "--integrity", "sha512-" + base64.b64encode(hashlib.sha512(environment.read_bytes()).digest()).decode(),
                   "--registry-dir", args.registry, "--startup-timeout", "30", "--timeout", "2", "--allow", "build"]
        endpoint = root / "forced-window"
        # A private temporary directory isolates the native profile, which the forced exit must still remove. It is
        # created in the system temporary directory, not under the work path: Chromium puts its singleton Unix socket
        # there, and sockaddr_un limits that path to about 108 bytes whatever the checkout's depth.
        with profile_directory() as profile_root:
            profiles = pathlib.Path(profile_root).resolve()
            forced_environment = dict(os.environ, TMPDIR=str(profiles), TEMP=str(profiles), TMP=str(profiles))
            with (pathlib.Path(args.work) / "watchdog.log").open("w") as log:
                client = Client([args.agent, *command, "--mcp", "--listen", str(endpoint)], log, env=forced_environment)
                try:
                    client.initialize()
                    client.start("build")
                    try:
                        status = client.child.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        raise AssertionError(f"The forced exit did not happen\n{pathlib.Path(log.name).read_text()}") from None
                    assert status == 71, f"exit {status}\n{pathlib.Path(log.name).read_text()}"
                    assert isinstance(client.messages.get(timeout=2), EOFError), "A renderer retained MCP stdout"
                finally:
                    client.close()
            assert (project / "renderer.started").is_file(), "The deadline expired before the renderer actually blocked"
            assert not endpoint.exists(), "The forced exit left its attachment capability behind"
            left, contents = remaining_profiles(profiles)
            assert not left, f"The forced exit left its native profile behind: {left} {contents}\n{pathlib.Path(log.name).read_text()}"
        # This same-path relaunch, through the CLI, must reach its authenticated page before the intentional error.
        # It checks endpoint reuse and Unicode failure reporting in one process; normal builds
        # are exercised by the functional MCP scenario.
        failed_environment = root / "failed-bootstrap.tgz"
        # From the original archive: the blocked one also delays its startup by three seconds.
        with tarfile.open(args.environment, "r:gz") as archive, tarfile.open(failed_environment, "w:gz", compresslevel=1) as output:
            for member in archive.getmembers():
                data = archive.extractfile(member).read() if member.isfile() else None
                if member.name == "package/runtime.mjs":
                    connect = re.findall(rb"([A-Za-z_$][A-Za-z0-9_$]*) as connectAutomation(?=[,}])", data)
                    assert len(connect) == 1
                    data += b"\n" + connect[0] + b"=()=>{throw new Error('B'+String.fromCodePoint(0xe9).repeat(1022)+String.fromCodePoint(0x1f600)+'tail');};"
                    member.size = len(data)
                output.addfile(member, io.BytesIO(data) if data is not None else None)
        failed_command = list(command)
        failed_command[failed_command.index("--environment") + 1] = str(failed_environment)
        failed_command[failed_command.index("--integrity") + 1] = "sha512-" + base64.b64encode(hashlib.sha512(failed_environment.read_bytes()).digest()).decode()
        failed = subprocess.run([args.agent, *failed_command, "--listen", str(endpoint), "--operation", "build"],
                                capture_output=True, text=True, encoding="utf-8", timeout=45)
        assert failed.returncode == 70, (failed.returncode, failed.stdout, failed.stderr)
        assert "Automation startup: B" + "é" * 511 + "\n" in failed.stderr, failed.stderr
        assert not endpoint.exists()
    print(f"{args.scenario}: blocked renderer, native deadline, EOF and private endpoint cleanup passed")


def consumer(args):
    """An application that opted in with enableAutomation (#501), driven from another process.

    The window and its `consumer.render` callback belong to automation_consumer; this process only attaches. The preview
    runs in the application's own window, so its page reaches the application's C++ callback, which a window owned by
    the agent never could."""
    integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="consumer-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        base = ["--project", str(project), "--environment", args.environment, "--integrity", integrity,
                "--registry-dir", args.registry]
        installed = subprocess.run([args.driver, *base, "--", "install"], capture_output=True, text=True, timeout=120)
        assert installed.returncode == 0, installed.stdout + installed.stderr
        endpoint, sessions, called = root / "endpoint", root / "sessions", root / "called.txt"
        with profile_directory() as profile_root, (root / "consumer.log").open("w") as log:
            owner_environment = dict(os.environ, TMPDIR=profile_root, TEMP=profile_root, TMP=profile_root)
            owner = subprocess.Popen([args.consumer, *base, "--listen", str(endpoint), "--sessions", str(sessions),
                                      "--called", str(called)],
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=log, text=True, env=owner_environment)
            try:
                deadline = time.monotonic() + 60
                while not (endpoint / "endpoint.json").exists():
                    assert owner.poll() is None, (root / "consumer.log").read_text()
                    assert time.monotonic() < deadline, "The application never published its endpoint"
                    time.sleep(0.05)
                # The endpoint is published before the discovery record. Wait for the
                # record as well, polling without the private attachment capability.
                token = json.loads((endpoint / "endpoint.json").read_text())["token"]
                while True:
                    listed = subprocess.run([args.agent, "--list", "--sessions-dir", str(sessions)],
                                            capture_output=True, text=True, timeout=10)
                    assert listed.returncode == 0, listed.stderr
                    assert token not in listed.stdout + listed.stderr
                    entries = json.loads(listed.stdout)["sessions"]
                    if entries:
                        break
                    assert owner.poll() is None, (root / "consumer.log").read_text()
                    assert time.monotonic() < deadline, f"The application never registered its session: {listed.stdout}"
                    time.sleep(0.05)
                assert len(entries) == 1 and entries[0]["state"] == "running", listed.stdout
                assert sorted(entries[0]["allow"]) == sorted(["check", "build", "preview", "inspect", "interact", "call"]), entries
                assert entries[0]["callbacks"] == ["consumer.render"], entries
                assert pathlib.Path(entries[0]["endpoint"]).resolve() == endpoint.resolve(), entries
                configuration = subprocess.run([args.agent, "--print-mcp-config", "--attach", str(endpoint)],
                                               capture_output=True, text=True, timeout=10)
                assert configuration.returncode == 0, configuration.stderr
                server = json.loads(configuration.stdout)["mcpServers"]["webfront"]
                assert server["args"][0] == "--attach" and server["args"][-1] == "--mcp"
                assert token not in configuration.stdout
                client = Client([args.agent, *server["args"]], log)
                try:
                    client.initialize()
                    # The grants are the application's: the agent cannot widen them.
                    for operation in ("test", "init", "install", "embed"):
                        client.tool("start", {"session": client.session, "operation": operation}, error=True)
                    # Only the allow-listed callback can be called, and the page can never reach webfront.* ones.
                    for callback in ("consumer.secret", "webfront.developer.read", "consumer.render.extra"):
                        client.tool("start", {"session": client.session, "operation": "call", "arguments": {"callback": callback}}, error=True)
                    client.run("check")
                    client.run("build")
                    assert (project / ".webfront/developer/build/app.js").is_file()
                    client.run("preview")
                    for _ in range(100):
                        run = client.wait(client.start("inspect"))
                        if run["state"] == "succeeded" and any(item["id"] == "render" for item in run["result"]["data"]["elements"]):
                            break
                        time.sleep(0.1)
                    snapshot = run["result"]["data"]
                    assert any(item["id"] == "render" for item in snapshot["elements"]), run
                    client.run("click", {"pageId": snapshot["pageId"], "elementId": "render"})
                    for _ in range(100):
                        updated = client.run("inspect")["data"]
                        if any(item["id"] == "result" and item["text"] == "rendered:hello" for item in updated["elements"]):
                            break
                        time.sleep(0.1)
                    else:
                        raise AssertionError(updated)
                    # The callback that answered is the application's own, in its own process.
                    answered = client.run("call", {"callback": "consumer.render", "value": "from-agent"})
                    assert answered["data"] == {"callback": "consumer.render", "result": "rendered:from-agent"}, answered
                    assert called.read_text().split() == ["hello", "from-agent"], called.read_text()
                finally:
                    client.close()
                assert owner.poll() is None, "Detaching an agent closed the application's window"
                shutdown = subprocess.run([args.agent, "--attach", str(endpoint), "--shutdown"],
                                          capture_output=True, text=True, timeout=10)
                assert shutdown.returncode == 0, shutdown.stderr
                status = owner.wait(timeout=30)
                assert status == 0, f"exit {status}\n{(root / 'consumer.log').read_text()}"
                assert not endpoint.exists() or not (endpoint / "endpoint.json").exists(), "The endpoint outlived the application"
                assert json.loads(subprocess.run([args.agent, "--list", "--sessions-dir", str(sessions)],
                                                 capture_output=True, text=True, timeout=10).stdout)["sessions"] == []
            except BaseException:
                print((root / "consumer.log").read_text(), flush=True)
                raise
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait()
    print("consumer: attach to an application's own window, its own callback, grants, discovery, shutdown passed")


def raw_request(port, text):
    """Sends raw bytes to a loopback port and returns the status code of the answer."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as peer:
        peer.sendall(text.encode())
        answer = b""
        while chunk := peer.recv(4096):
            answer += chunk
    return int(answer.split(b" ", 2)[1])


def consumer_http(args):
    """Two applications that enabled automation with `.http = true`, side by side (ADR-025, #632).

    Each serves MCP from its own process on its own loopback port. This process reads each instance's record and private
    mcp.json, never runs webfront-agent to operate them, and drives each window through its URL only."""
    integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="consumer-http-") as temporary:
        root = pathlib.Path(temporary)
        sessions = root / "sessions"
        instances = []
        with profile_directory() as profile_root, (root / "consumers.log").open("w") as log:
            try:
                for name in ("first", "second"):
                    project = root / name
                    shutil.copytree(args.fixture, project)
                    base = ["--project", str(project), "--environment", args.environment, "--integrity", integrity,
                            "--registry-dir", args.registry]
                    installed = subprocess.run([args.driver, *base, "--", "install"], capture_output=True, text=True, timeout=120)
                    assert installed.returncode == 0, installed.stdout + installed.stderr
                    # Each instance has its own browser profile, as two applications of a machine would.
                    profile = pathlib.Path(profile_root) / name
                    environment = dict(os.environ, TMPDIR=profile_root, TEMP=profile_root, TMP=profile_root,
                                       XDG_DATA_HOME=str(profile / "data"), XDG_CACHE_HOME=str(profile / "cache"),
                                       WEBFRONT_TEST_CEF_PROFILE=str(profile / "cef"))
                    called = root / f"{name}-called.txt"
                    owner = subprocess.Popen([args.consumer, *base, "--transport", "http", "--sessions", str(sessions),
                                              "--called", str(called)],
                                             stdin=subprocess.DEVNULL, stdout=log, stderr=log, text=True, env=environment)
                    instances.append({"name": name, "project": project, "owner": owner, "called": called})
                # Discovery: each record names its project, its URL and its private configuration, never its token.
                deadline = time.monotonic() + 60
                while True:
                    records = [json.loads(path.read_text()) for path in sessions.glob("*.json")] if sessions.is_dir() else []
                    if len(records) == 2 and all(pathlib.Path(record["configuration"]).exists() for record in records):
                        break
                    for instance in instances:
                        assert instance["owner"].poll() is None, (root / "consumers.log").read_text()
                    assert time.monotonic() < deadline, f"The applications never published their MCP URLs: {records}"
                    time.sleep(0.05)
                if os.name != "nt":
                    assert sessions.stat().st_mode & 0o077 == 0
                for instance in instances:
                    record = next(record for record in records if pathlib.Path(record["project"]).resolve() == instance["project"].resolve())
                    configuration = pathlib.Path(record["configuration"])
                    assert configuration.parent == pathlib.Path(record["endpoint"]) == sessions / record["session"], record
                    if os.name != "nt":
                        assert configuration.stat().st_mode & 0o077 == 0 and configuration.parent.stat().st_mode & 0o077 == 0
                    instance.update(record=record, client=HttpClient(configuration),
                                    token=json.loads(configuration.read_text())["mcpServers"]["webfront"]["headers"]["Authorization"][7:])
                    assert instance["client"].url.geturl() == record["mcp"], record
                    assert instance["token"] not in json.dumps(records) + (root / "consumers.log").read_text()
                first, second = instances
                assert first["client"].url.port != second["client"].url.port
                # The listener of the first refuses what a web page, a rebinding name or a wrong token sends.
                port = first["client"].url.port
                body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
                request = ("POST /mcp HTTP/1.1\r\nHost: {host}\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\n"
                           "Accept: application/json\r\nContent-Length: {length}\r\n{extra}\r\n{body}")
                def status(host=f"127.0.0.1:{port}", token=first["token"], extra=""):
                    return raw_request(port, request.format(host=host, token=token, extra=extra, length=len(body), body=body))
                assert status(token=second["token"]) == 401
                assert status(token="0" * 64) == 401
                assert status(host=f"rebinding.example:{port}") == 421
                assert status(extra="Origin: http://127.0.0.1:9002\r\n") == 403
                assert status(extra="Sec-Fetch-Site: cross-site\r\n") == 403
                assert raw_request(port, f"GET /WebFront.js HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n") == 404
                # Each agent reaches its own instance: the session names that instance's project.
                for instance in instances:
                    client = instance["client"]
                    client.initialize()
                    assert pathlib.Path(client.tool("session")["project"]).resolve() == instance["project"].resolve()
                client = first["client"]
                for operation in ("test", "init", "install", "embed"):
                    client.tool("start", {"session": client.session, "operation": operation}, error=True)
                client.tool("start", {"session": second["client"].session, "operation": "check"}, error=True)
                client.run("check")
                client.run("build")
                assert (first["project"] / ".webfront/developer/build/app.js").is_file()
                assert not (second["project"] / ".webfront/developer/build/app.js").exists()
                answered = client.run("call", {"callback": "consumer.render", "value": "from-http"})
                assert answered["data"] == {"callback": "consumer.render", "result": "rendered:from-http"}, answered
                assert first["called"].read_text().split() == ["from-http"]
                assert not second["called"].exists()
                second["client"].run("check")
                for instance in instances:
                    instance["client"].close()
                    assert instance["owner"].poll() is None, "Ending an MCP session closed the application's window"
                # Closing the applications withdraws their URLs, private directories and records.
                for instance in instances:
                    endpoint = instance["record"]["endpoint"]
                    shutdown = subprocess.run([args.agent, "--attach", endpoint, "--shutdown"], capture_output=True, text=True, timeout=10)
                    assert shutdown.returncode == 0, shutdown.stderr
                    status_code = instance["owner"].wait(timeout=30)
                    assert status_code == 0, f"exit {status_code}\n{(root / 'consumers.log').read_text()}"
                    assert not pathlib.Path(endpoint).exists(), "The private directory outlived the application"
                    with contextlib.suppress(ConnectionRefusedError):
                        raw_request(instance["client"].url.port, "POST /mcp HTTP/1.1\r\n\r\n")
                        raise AssertionError("The MCP listener outlived the application")
                assert list(sessions.glob("*.json")) == []
            except BaseException:
                print((root / "consumers.log").read_text(), flush=True)
                raise
            finally:
                for instance in instances:
                    if instance["owner"].poll() is None:
                        instance["owner"].kill()
                        instance["owner"].wait()
    print("consumer-http: two applications serve their own MCP URLs, refusals, operations, callbacks and cleanup passed")


def main():
    parser = argparse.ArgumentParser()
    for name in ("agent", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--scenario", choices=("session", "mcp-watchdog", "consumer", "consumer-http"), default="session")
    parser.add_argument("--consumer", help="the automation_consumer executable of the consumer scenario")
    args = parser.parse_args()
    if args.scenario == "consumer":
        consumer(args)
        return
    if args.scenario == "consumer-http":
        consumer_http(args)
        return
    if args.scenario != "session":
        watchdog(args)
        return
    integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="session-") as temporary:
        check_result_backoff(args.agent, pathlib.Path(temporary))
        check_startup_backoff(args.agent, pathlib.Path(temporary))
        check_startup_backoff(args.agent, pathlib.Path(temporary), ready_delay=1.25)
        check_startup_backoff(args.agent, pathlib.Path(temporary), ready_delay=3)
        project = pathlib.Path(temporary) / "project"
        shutil.copytree(args.fixture, project)
        command = ["--project", str(project), "--environment", args.environment, "--integrity", integrity,
                   "--registry-dir", args.registry]
        installed = subprocess.run([args.driver, *command, "--", "install"], capture_output=True, text=True, timeout=120)
        assert installed.returncode == 0, installed.stdout + installed.stderr
        with (pathlib.Path(temporary) / "agent.log").open("w") as log:
            client = Client([args.agent, *command, "--mcp", "--timeout", "20",
                             *sum((["--allow", op] for op in ("check", "build", "test", "preview", "inspect", "interact")), [])], log)
            try:
                client.initialize()
                client.tool("start", {"session": "wrong", "operation": "build"}, error=True)
                client.tool("start", {"session": client.session, "operation": "exec"}, error=True)
                client.tool("start", {"session": client.session, "operation": "build", "arguments": {"entry": "../escape"}}, error=True)
                source = project / "main.ts"
                original = source.read_text()
                source.write_text("const value: = ;")
                failed = client.run("build", state="failed")
                assert failed.get("diagnostics"), failed
                assert failed["diagnostics"][0]["file"] == "main.ts", failed
                source.write_text("const value: number = 'wrong';")
                checked = client.run("check", state="failed")
                assert checked.get("diagnostics"), checked
                source.write_text(original)
                client.run("check")
                client.run("build")
                assert (project / ".webfront/developer/build/app.js").is_file()
                client.run("test", {"pattern": "passes"})
                unicode_failure = client.run("test", {"pattern": "no-spec-é😀"}, state="failed")
                assert unicode_failure["report"]["pattern"] == "no-spec-é😀", unicode_failure
                client.run("preview")
                for attempt in range(50):
                    snapshot_run = client.wait(client.start("inspect"))
                    if snapshot_run["state"] == "succeeded":
                        break
                    time.sleep(0.1)
                assert snapshot_run["state"] == "succeeded", snapshot_run
                snapshot = snapshot_run["result"]["data"]
                assert any(item["id"] == "increment" for item in snapshot["elements"]), snapshot
                client.run("click", {"pageId": snapshot["pageId"], "elementId": "increment"})
                updated = client.run("inspect")["data"]
                assert any(item["id"] == "value" and item["text"] == "1" for item in updated["elements"]), updated
                assert any(item["id"] == "authorization" and item["text"] == "denied" for item in updated["elements"]), updated
                client.run("click", {"pageId": updated["pageId"], "elementId": "probe-wire"})
                for _ in range(50):
                    proof = client.run("inspect")["data"]
                    if any(item["id"] == "wire-authorization" and item["text"] == "wire forgery denied" for item in proof["elements"]):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError(proof)
                client.run("fill", {"pageId": updated["pageId"], "elementId": "name", "value": "secret-not-to-export"})
                assert "secret-not-to-export" not in json.dumps(client.run("inspect"))
                client.run("preview")
                client.run("click", {"pageId": snapshot["pageId"], "elementId": "increment"}, state="failed")
                run = client.start("test", {"pattern": "waits for cancellation"})
                # Wait for dispatch before cancellation; the unit suite also covers cancellation while queued.
                for attempt in range(100):
                    if client.result(run)["state"] == "running":
                        break
                    time.sleep(0.02)
                client.tool("cancel", {"session": client.session, "runId": run})
                cancelled = client.wait(run)
                assert cancelled["state"] == "cancelled", f"Unexpected run state: {cancelled}"
                client.run("test", {"pattern": "passes"})
            finally:
                client.close()
            assert client.child.returncode == 0, (client.child.returncode, pathlib.Path(log.name).read_text())

        # An agent builds a project from nothing: the starter comes from the environment's manifest and is served by the launcher.
        setup = pathlib.Path(temporary) / "setup-project"
        setup.mkdir()
        setup_command = ["--project", str(setup), "--environment", args.environment, "--integrity", integrity,
                         "--registry-dir", args.registry]
        with (pathlib.Path(temporary) / "setup.log").open("w") as log:
            client = Client([args.agent, *setup_command, "--mcp", "--timeout", "60", "--startup-timeout", "60",
                             *sum((["--allow", op] for op in ("init", "create", "install", "restore", "embed")), [])], log)
            try:
                client.initialize()
                missing = client.run("create", {"starter": "nonexistent"}, state="failed")
                assert "Unknown pinned starter" in missing["output"], missing
                client.run("create", {"starter": "life", "directory": "ui"})
                assert (setup / "ui/main.tsx").is_file() and (setup / "ui/style.css").is_file()
                assert "--print-mcp-config" in (setup / "ui/AGENTS.md").read_text(encoding="utf-8"), "The starter must explain how an agent attaches"
                again = client.run("create", {"starter": "life", "directory": "ui"}, state="failed")
                assert (setup / "ui/main.tsx").is_file(), again
                client.run("init")
                assert (setup / "webfront.project.json").is_file()
                client.run("install", {"package": "react@19.3.1"})
                client.run("install", {"package": "react-dom@19.3.1"})
                client.run("restore")
                client.run("embed", {"entry": "ui/main.tsx", "output": "generated/LifeUI.hpp"})
                header = setup / "generated/LifeUI.hpp"
                assert header.is_file() and b"class LifeUI" in header.read_bytes(), "embed did not write the provider"
            finally:
                client.close()
            assert client.child.returncode == 0, (client.child.returncode, pathlib.Path(log.name).read_text())

        # A human-owned window is already running before either agent attaches. Native grants remain the owner's.
        endpoint = pathlib.Path(temporary) / "shared-window"
        # Keep Chromium's temporary Unix socket paths below sockaddr_un's limit, independently
        # of the checkout/work path length. The agent still creates its own private child profile.
        with profile_directory() as profile_root, (pathlib.Path(temporary) / "shared.log").open("w") as log:
            profiles = pathlib.Path(profile_root).resolve()
            owner_environment = dict(os.environ, TMPDIR=str(profiles), TEMP=str(profiles), TMP=str(profiles))
            owner = subprocess.Popen([args.agent, *command, "--serve", "--listen", str(endpoint),
                                      "--allow", "build", "--allow", "test", "--timeout", "20"],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=log, text=True, env=owner_environment)
            try:
                deadline = time.monotonic() + 20
                while not (endpoint / "endpoint.json").exists():
                    assert owner.poll() is None, pathlib.Path(log.name).read_text()
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                published = json.loads((endpoint / "endpoint.json").read_text())
                if os.name != "nt":
                    assert endpoint.stat().st_mode & 0o077 == 0
                    assert (endpoint / "endpoint.json").stat().st_mode & 0o077 == 0
                with socket.create_connection(("127.0.0.1", published["port"]), timeout=3) as peer:
                    peer.sendall(b'GET / HTTP/1.1\r\n\r\n')
                    response = peer.recv(4096)
                    assert b'error' in response and published["token"].encode() not in response
                bad = pathlib.Path(temporary) / "wrong-capability"
                bad.mkdir(mode=0o700)
                bad_endpoint = {**published, "token": "0" * 64}
                (bad / "endpoint.json").write_text(json.dumps(bad_endpoint))
                (bad / "endpoint.json").chmod(0o600)
                denied = subprocess.run([args.agent, "--attach", str(bad), "--operation", "build"],
                                        capture_output=True, text=True, timeout=10)
                assert denied.returncode != 0 and published["token"] not in denied.stdout + denied.stderr
                first = Client([args.agent, "--attach", str(endpoint), "--mcp"], log)
                second = Client([args.agent, "--attach", str(endpoint), "--mcp"], log)
                try:
                    first.initialize()
                    second.initialize()
                    assert first.session == second.session
                    first.run("build")
                    first.tool("start", {"session": first.session, "operation": "preview"}, error=True)
                    run = first.start("test", {"pattern": "waits for cancellation"})
                    second.tool("start", {"session": second.session, "operation": "build"}, error=True)
                    second.tool("cancel", {"session": second.session, "runId": run})
                    cancelled = first.wait(run)
                    assert cancelled["state"] == "cancelled", f"Unexpected run state: {cancelled}"
                finally:
                    first.close()
                    second.close()
                assert owner.poll() is None, "Detaching an agent closed the human's window"
                cli = subprocess.run([args.agent, "--attach", str(endpoint), "--operation", "build"],
                                     capture_output=True, text=True, timeout=30)
                assert cli.returncode == 0 and json.loads(cli.stdout)["state"] == "succeeded", cli.stdout + cli.stderr
                assert list(profiles.glob("webfront-agent-*")), "The test must observe the actual native profile"
                if os.name == "posix":
                    assert owner.poll() is None, f"Owner exited before SIGTERM: {owner.returncode}\n{pathlib.Path(log.name).read_text()}"
                    owner.send_signal(signal.SIGTERM)
                    status = owner.wait(timeout=15)
                    assert status == 128 + signal.SIGTERM, f"exit {status}\n{pathlib.Path(log.name).read_text()}"
                else:
                    shutdown = subprocess.run([args.agent, "--attach", str(endpoint), "--shutdown"],
                                              capture_output=True, text=True, timeout=10)
                    assert shutdown.returncode == 0, shutdown.stderr
                    # Before it exits, the owner waits up to 5 s for the WebView2 browser, ends it, then removes the profile.
                    assert owner.wait(timeout=30) == 0, pathlib.Path(log.name).read_text()
                # Only the owner removes the profile, at the latest just after it has exited.
                left, contents = remaining_profiles(profiles)
                assert not left, f"The owner left its native profile behind: {left} {contents}\n{pathlib.Path(log.name).read_text()}"
                assert not endpoint.exists(), "The owner left its attachment capability behind"
                unavailable = subprocess.run([args.agent, "--attach", str(endpoint), "--operation", "build"],
                                             capture_output=True, text=True, timeout=5)
                assert unavailable.returncode == 69, unavailable.stderr
            except BaseException:
                print(pathlib.Path(log.name).read_text(), flush=True)
                raise
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait()

    print("MCP real-window regression passed: diagnostics/correction/check/build/test/preview/actions/cancellation/authenticated shared-window attachment")


if __name__ == "__main__":
    main()
