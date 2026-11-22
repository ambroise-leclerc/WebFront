"""Real MCP client driving applications that serve MCP themselves (ADR-025), with no third-party Python modules.

Each scenario launches automation_host, an application that called enableAutomation, reads its discovery record and its
private mcp.json, and drives its window through its Streamable HTTP URL only."""
import argparse
import base64
import contextlib
import hashlib
import http.client
import io
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse


class Client:
    """MCP over the Streamable HTTP listener of an application, from its private mcp.json."""

    def __init__(self, configuration, timeout=30):
        self.timeout = timeout  # seconds for the session to become ready and for one operation to finish
        server = json.loads(pathlib.Path(configuration).read_text())["mcpServers"]["webfront"]
        assert server["type"] == "http", server
        self.url = urllib.parse.urlsplit(server["url"])
        assert self.url.hostname == "127.0.0.1" and self.url.path == "/mcp", server["url"]
        self.authorization = server["headers"]["Authorization"]
        self.token = self.authorization[len("Bearer "):]
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

    def tool(self, name, arguments=None, error=False):
        result = self.send("tools/call", {"name": "webfront." + name, "arguments": arguments or {}})
        assert result.get("isError", False) == error, result
        assert json.loads(result["content"][0]["text"]) == result["structuredContent"]
        return result["structuredContent"]

    def initialize(self):
        result = self.send("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                          "clientInfo": {"name": "WebFront-regression", "version": "1"}})
        assert result["protocolVersion"] == "2025-11-25", result
        assert "webfront.session" in result["instructions"], result
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
        """Terminates the MCP session only: the application and its window stay."""
        if self.mcp_session:
            status, _, _ = self.post(None, {"Content-Type": None, "Accept": None}, method="DELETE")
            assert status == 204, status
            self.mcp_session = None


def remove_profile(path, attempts=20, delay=0.1, remove=shutil.rmtree):
    """Removes a browser profile that a helper of the browser may still write into after the application exited (#642).

    The helper can create a file between the walk of the directory and its removal (ENOTEMPTY): the removal is repeated
    until the helper is gone, and what it still holds at the end is left to the system's temporary directory."""
    for _ in range(attempts):
        # A FileNotFoundError may come from a file that the helper removed during the walk, not from the root: only
        # the absence of the root ends the removal.
        with contextlib.suppress(OSError):
            remove(path)
        if not os.path.exists(path):
            return True
        time.sleep(delay)
    shutil.rmtree(path, ignore_errors=True)
    return not os.path.exists(path)


@contextlib.contextmanager
def profile_directory():
    """A private directory in the system temporary directory, whose removal tolerates what the browser still removes.

    tempfile.TemporaryDirectory takes ignore_cleanup_errors only from Python 3.10. On Linux, the directory is in /tmp
    rather than an inherited TMPDIR, which may be long: Chromium's singleton socket path must stay below sockaddr_un's
    limit."""
    root = "/tmp" if sys.platform.startswith("linux") and os.access("/tmp", os.W_OK | os.X_OK) else None
    path = tempfile.mkdtemp(prefix="wf-profile- &%!^ 雪 " if sys.platform == "win32" else "wf-profile-", dir=root)
    try:
        yield path
    finally:
        remove_profile(path)


def raw_request(port, text):
    """Sends raw bytes to a loopback port and returns the status code of the answer."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as peer:
        peer.sendall(text.encode())
        answer = b""
        while chunk := peer.recv(4096):
            answer += chunk
    return int(answer.split(b" ", 2)[1])


class Host:
    """automation_host: an application that enabled automation, launched with its own browser profile."""

    def __init__(self, args, project, root, name, log, extra=(), environment=None, integrity=None):
        self.root, self.name, self.log = root, name, log
        self.sessions = root / "sessions"
        # The profile is outside the scenario's directory, whose strict cleanup a late write of a browser helper must not
        # fail (#642): it lives in the private directory of profile_directory(), removed by remove_profile().
        self.profiles = profile_directory()
        temporary = self.profiles.__enter__()
        profile = pathlib.Path(temporary) / f"{name}-profile"
        self.base = ["--project", str(project), "--environment", environment or args.environment,
                     "--integrity", integrity or json.loads(pathlib.Path(args.provision).read_text())["integrity"],
                     "--registry-dir", args.registry]
        env = dict(os.environ, XDG_DATA_HOME=str(profile / "data"), XDG_CACHE_HOME=str(profile / "cache"),
                   WEBFRONT_TEST_CEF_PROFILE=str(profile / "cef"))
        # Chromium puts its singleton Unix socket in TMPDIR: a short private directory keeps it below sockaddr_un's limit.
        env.update(TMPDIR=temporary, TEMP=temporary, TMP=temporary)
        # Closing its standard input closes the application's window (stop()).
        self.process = subprocess.Popen([args.host, *self.base, "--sessions", str(self.sessions), *extra],
                                        stdin=subprocess.PIPE, stdout=log, stderr=log, text=True, env=env)
        self.project = pathlib.Path(project).resolve()

    def full_log(self):
        return pathlib.Path(self.log.name).read_text(encoding="utf-8", errors="replace")

    def logs(self):
        """The end of the log, for diagnostics; assertions on its whole content use full_log()."""
        return self.full_log()[-4000:]

    def record(self, timeout=60):
        """This instance's discovery record (published by this process), once its client configuration exists."""
        deadline = time.monotonic() + timeout
        while True:
            for path in self.sessions.glob("*.json") if self.sessions.is_dir() else []:
                with contextlib.suppress(ValueError, OSError):
                    record = json.loads(path.read_text())
                    if (record["pid"] == self.process.pid and pathlib.Path(record["project"]).resolve() == self.project
                            and pathlib.Path(record["configuration"]).exists()):
                        return record
            assert self.process.poll() is None, f"{self.name} exited with {self.process.returncode}\n{self.logs()}"
            assert time.monotonic() < deadline, f"{self.name} never published its MCP URL\n{self.logs()}"
            time.sleep(0.05)

    def client(self, timeout=30):
        return Client(self.record()["configuration"], timeout)

    def stop(self):
        """Closes the window as its owner would; the application then exits with status 0."""
        with contextlib.suppress(OSError):
            self.process.stdin.close()

    def wait(self, timeout=30):
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"{self.name} did not exit\n{self.logs()}") from None

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.profiles.__exit__(None, None, None)


def check_withdrawn(record, port, sessions):
    """After the application ended: no record in `sessions` (the host's --sessions), no private directory, no listener."""
    sessions = pathlib.Path(sessions)
    assert not pathlib.Path(record["endpoint"]).exists(), "The private directory outlived the application"
    assert not (sessions / (record["session"] + ".json")).exists(), "The record outlived the application"
    with contextlib.suppress(ConnectionRefusedError):
        raw_request(port, "POST /mcp HTTP/1.1\r\n\r\n")
        raise AssertionError("The MCP listener outlived the application")


def install(args, project):
    integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
    command = ["--project", str(project), "--environment", args.environment, "--integrity", integrity, "--registry-dir", args.registry]
    installed = subprocess.run([args.driver, *command, "--", "install"], capture_output=True, text=True, timeout=120)
    assert installed.returncode == 0, installed.stdout + installed.stderr


def watchdog(args):
    """Exercise native revocation with a real blocked renderer, without compiling Jasmine first.

    Only the test archive replaces the operation body. The real signed callback, runner, native deadline, window close
    grace, unresponsive-window exit and the withdrawal of the listener, record and private directory stay on the
    production path of enableAutomation. The application is driven through its MCP URL, the way agents drive it.
    """
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="watchdog-") as temporary:
        root = pathlib.Path(temporary)
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
        integrity = "sha512-" + base64.b64encode(hashlib.sha512(environment.read_bytes()).digest()).decode()
        directory = root / "forced-window"
        options = ["--directory", str(directory), "--grants", "build", "--timeout", "2", "--startup-timeout", "30"]
        with (pathlib.Path(args.work) / "watchdog.log").open("w") as log:
            host = Host(args, project, root, "blocked", log, options, str(environment), integrity)
            try:
                record = host.record()
                client = host.client()
                client.initialize()
                client.start("build")
                status = host.wait(timeout=30)
                # The deadline revokes the session and asks the window to close: a window that closes ends show() (71); one
                # whose renderer keeps it open is ended by enableAutomation after its grace (72).
                assert status in (71, 72), f"exit {status}\n{host.logs()}"
            finally:
                host.close()
        assert (project / "renderer.started").is_file(), "The deadline expired before the renderer actually blocked"
        check_withdrawn(record, client.url.port, host.sessions)
        # A relaunch on the same private directory must reach its authenticated page before the intentional error. It
        # checks the reuse of the directory and Unicode failure reporting in one process; normal builds are exercised by
        # the functional scenario.
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
        failed_integrity = "sha512-" + base64.b64encode(hashlib.sha512(failed_environment.read_bytes()).digest()).decode()
        with (pathlib.Path(args.work) / "watchdog-failed.log").open("w", encoding="utf-8") as log:
            host = Host(args, project, root, "failed", log, options, str(failed_environment), failed_integrity)
            try:
                assert host.wait(timeout=45) == 70, host.logs()
            finally:
                host.close()
            assert "Automation startup: B" + "é" * 511 + "\n" in host.logs(), host.logs()
        assert not directory.exists()
    print(f"{args.scenario}: blocked renderer, native deadline, unresponsive window and private directory cleanup passed")


def consumer(args):
    """Two applications that enabled automation, side by side (#501, ADR-025, #632).

    Each serves MCP from its own process on its own loopback port. This process reads each instance's record and private
    mcp.json and drives each window through its URL only. The preview runs in the application's own window, so its page
    reaches the application's C++ callback."""
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="consumer-") as temporary:
        root = pathlib.Path(temporary)
        sessions = root / "sessions"
        hosts = []
        with (root / "consumers.log").open("w") as log:
            try:
                for name in ("first", "second"):
                    project = root / name
                    shutil.copytree(args.fixture, project)
                    install(args, project)
                    host = Host(args, project, root, name, log, ["--called", str(root / f"{name}-called.txt")])
                    host.called = root / f"{name}-called.txt"
                    hosts.append(host)
                # Discovery: each record names its project, its URL and its private configuration, never its token.
                for host in hosts:
                    host.entry = host.record()
                    configuration = pathlib.Path(host.entry["configuration"])
                    assert configuration.parent == pathlib.Path(host.entry["endpoint"]) == sessions / host.entry["session"], host.entry
                    assert sorted(host.entry["allow"]) == sorted(["check", "build", "preview", "inspect", "interact", "call"]), host.entry
                    assert host.entry["callbacks"] == ["consumer.render"], host.entry
                    if os.name != "nt":
                        assert configuration.stat().st_mode & 0o077 == 0 and configuration.parent.stat().st_mode & 0o077 == 0
                        assert sessions.stat().st_mode & 0o077 == 0
                    host.mcp = Client(configuration)
                    assert host.mcp.url.geturl() == host.entry["mcp"], host.entry
                records = "".join(path.read_text() for path in sessions.glob("*.json"))
                for host in hosts:
                    assert host.mcp.token not in records + host.full_log()
                first, second = hosts
                assert first.mcp.url.port != second.mcp.url.port
                # The listener of the first refuses what a web page, a rebinding name or a wrong token sends.
                port = first.mcp.url.port
                body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
                request = ("POST /mcp HTTP/1.1\r\nHost: {host}\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\n"
                           "Accept: application/json\r\nContent-Length: {length}\r\n{extra}\r\n{body}")
                def status(host=f"127.0.0.1:{port}", token=first.mcp.token, extra=""):
                    return raw_request(port, request.format(host=host, token=token, extra=extra, length=len(body), body=body))
                assert status(token=second.mcp.token) == 401
                assert status(token="0" * 64) == 401
                assert status(host=f"rebinding.example:{port}") == 421
                assert status(extra="Origin: http://127.0.0.1:9002\r\n") == 403
                assert status(extra="Sec-Fetch-Site: cross-site\r\n") == 403
                assert raw_request(port, f"GET /WebFront.js HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n") == 404
                # Each agent reaches its own instance: the session names that instance's project.
                for host in hosts:
                    host.mcp.initialize()
                    assert pathlib.Path(host.mcp.tool("session")["project"]).resolve() == host.project
                client = first.mcp
                # The grants are the application's: the agent cannot widen them.
                for operation in ("test", "init", "install", "embed"):
                    client.tool("start", {"session": client.session, "operation": operation}, error=True)
                client.tool("start", {"session": second.mcp.session, "operation": "check"}, error=True)
                # Only the allow-listed callback can be called, and the page can never reach webfront.* ones.
                for callback in ("consumer.secret", "webfront.developer.read", "consumer.render.extra"):
                    client.tool("start", {"session": client.session, "operation": "call", "arguments": {"callback": callback}}, error=True)
                client.run("check")
                client.run("build")
                assert (first.project / ".webfront/developer/build/app.js").is_file()
                assert not (second.project / ".webfront/developer/build/app.js").exists()
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
                assert first.called.read_text().split() == ["hello", "from-agent"], first.called.read_text()
                assert not second.called.exists()
                second.mcp.run("check")
                for host in hosts:
                    host.mcp.close()
                    assert host.process.poll() is None, "Ending an MCP session closed the application's window"
                # Closing the applications withdraws their URLs, private directories and records.
                for host in hosts:
                    host.stop()
                    status_code = host.wait()
                    assert status_code == 0, f"exit {status_code}\n{host.logs()}"
                    check_withdrawn(host.entry, host.mcp.url.port, host.sessions)
                assert list(sessions.glob("*.json")) == []
            except BaseException:
                print((root / "consumers.log").read_text(), flush=True)
                raise
            finally:
                for host in hosts:
                    host.close()
    print("consumer: two applications serve their own MCP URLs, refusals, grants, preview, callbacks and cleanup passed")


def session(args):
    """The functional regression of one application window driven through its URL."""
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="session-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        install(args, project)
        with (root / "window.log").open("w") as log:
            host = Host(args, project, root, "window", log,
                        ["--grants", "check,build,test,preview,inspect,interact", "--timeout", "20"])
            try:
                record = host.record()
                client, other = host.client(), host.client()
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
                # A second client of the same application shares its session, its runs and its busy/cancel rules.
                other.initialize()
                assert other.session == client.session
                run = client.start("test", {"pattern": "waits for cancellation"})
                for attempt in range(100):
                    if client.result(run)["state"] == "running":
                        break
                    time.sleep(0.02)
                other.tool("start", {"session": other.session, "operation": "build"}, error=True)
                other.tool("cancel", {"session": other.session, "runId": run})
                cancelled = client.wait(run)
                assert cancelled["state"] == "cancelled", f"Unexpected run state: {cancelled}"
                client.run("test", {"pattern": "passes"})
                other.close()
                client.close()
                assert host.process.poll() is None, "Ending the MCP sessions closed the window"
                host.stop()
                assert host.wait(timeout=30) == 0, host.logs()
                check_withdrawn(record, client.url.port, host.sessions)
            except BaseException:
                print(host.logs(), flush=True)
                raise
            finally:
                host.close()

        # An agent builds a project from nothing: the starter comes from the environment's manifest, beside its archive.
        setup = root / "setup-project"
        setup.mkdir()
        with (root / "setup.log").open("w") as log:
            host = Host(args, setup, root, "setup", log,
                        ["--grants", "init,create,install,restore,embed", "--timeout", "60", "--startup-timeout", "60"])
            try:
                client = host.client(timeout=60)
                client.initialize()
                missing = client.run("create", {"starter": "nonexistent"}, state="failed")
                assert "Unknown pinned starter" in missing["output"], missing
                client.run("create", {"starter": "life", "directory": "ui"})
                assert (setup / "ui/main.tsx").is_file() and (setup / "ui/style.css").is_file()
                assert "mcp.json" in (setup / "ui/AGENTS.md").read_text(encoding="utf-8"), "The starter must explain how an agent connects"
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
                client.close()
                host.stop()
                assert host.wait(timeout=30) == 0, host.logs()
            except BaseException:
                print(host.logs(), flush=True)
                raise
            finally:
                host.close()
    print("MCP real-window regression passed: diagnostics/correction/check/build/test/preview/actions/cancellation/shared session/setup")


def stable(args):
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="stable-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        install(args, project)
        with (root / "stable.log").open("w") as log:
            # The reservation must be released before the host binds; retry only a confirmed EADDRINUSE.
            for attempt in range(5):
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    port = reservation.getsockname()[1]
                client, original = None, None
                for run in range(2):
                    host = Host(args, project, root, f"stable-{attempt}-{run}", log, ["--stable-mcp", "true", "--mcp-port", str(port)])
                    try:
                        try:
                            path = pathlib.Path(host.record()["configuration"])
                        except AssertionError:
                            if host.process.poll() == 73:
                                break
                            raise
                        if client is None:
                            original = path.read_bytes()
                            client = Client(path)
                        else:
                            assert path.read_bytes() == original
                            client.mcp_session = None
                        client.initialize()
                        assert client.tool("session")["session"] == client.session
                        client.close()
                        host.stop()
                        assert host.wait() == 0, host.logs()
                    finally:
                        host.close()
                else:
                    return
            raise AssertionError("MCP port was occupied on all five attempts")


def main():
    parser = argparse.ArgumentParser()
    for name in ("host", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--scenario", choices=("session", "mcp-watchdog", "consumer", "stable"), default="session")
    args = parser.parse_args()
    {"session": session, "mcp-watchdog": watchdog, "consumer": consumer, "stable": stable}[args.scenario](args)


if __name__ == "__main__":
    main()
