"""Real stdio MCP client driving an authorized developer window, with no third-party Python modules."""
import argparse
import json
import pathlib
import queue
import re
import shutil
import subprocess
import socket
import os
import base64
import hashlib
import io
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
            try:
                self.child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()
                raise
        self.reader.join(timeout=2)


def remaining_profiles(profiles, timeout=10):
    """The agent profiles left in `profiles`, once removed or after `timeout` seconds, with what they contain.

    On Linux, the CEF browser runs inside the agent and may write until it exits: the agent then leaves the
    last removal to a process that waits for its exit."""
    deadline = time.monotonic() + timeout
    while True:
        left = list(profiles.glob("webfront-agent-*"))
        if not left or time.monotonic() >= deadline:
            return left, [str(path.relative_to(profiles)) for profile in left for path in profile.rglob("*")][:40]
        time.sleep(0.05)


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
                    if args.scenario == "mcp-watchdog":
                        connect = re.findall(rb"([A-Za-z_$][A-Za-z0-9_$]*) as connectAutomation(?=[,}])", data)
                        assert len(connect) == 1
                        # A bridge RangeError consumes a JS sequence without reaching native code.
                        data += (b"\nconst beforeGap=" + connect[0] + b";" + connect[0] +
                                 b"=context=>{void context.call('poll','x'.repeat(65536)).then(()=>window.close(),()=>beforeGap(context));};")
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
        with tempfile.TemporaryDirectory(prefix="wf-profile-", ignore_cleanup_errors=True) as profile_root:
            profiles = pathlib.Path(profile_root).resolve()
            forced_environment = dict(os.environ, TMPDIR=str(profiles), TEMP=str(profiles), TMP=str(profiles))
            with (pathlib.Path(args.work) / "watchdog.log").open("w") as log:
                if args.scenario == "mcp-watchdog":
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
                else:
                    cli = subprocess.run([args.agent, *command, "--listen", str(endpoint), "--operation", "build"],
                                         stdout=subprocess.PIPE, stderr=log, text=True, timeout=45, env=forced_environment)
                    assert cli.returncode == 71, f"exit {cli.returncode}\n{cli.stdout}\n{pathlib.Path(log.name).read_text()}"
            assert (project / "renderer.started").is_file(), "The deadline expired before the renderer actually blocked"
            assert not endpoint.exists(), "The forced exit left its attachment capability behind"
            left, contents = remaining_profiles(profiles)
            assert not left, f"The forced exit left its native profile behind: {left} {contents}\n{pathlib.Path(log.name).read_text()}"
        if args.scenario == "cli-watchdog":
            # This same-path relaunch must reach its authenticated page before the intentional error.
            # It checks directory reuse and Unicode failure reporting in one process; normal builds
            # are exercised by the functional MCP/shared-window scenario.
            failed_environment = root / "failed-bootstrap.tgz"
            with tarfile.open(environment, "r:gz") as archive, tarfile.open(failed_environment, "w:gz", compresslevel=1) as output:
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
                                    capture_output=True, text=True, encoding="utf-8", timeout=15)
            assert failed.returncode == 70, (failed.returncode, failed.stdout, failed.stderr)
            assert "Automation startup: B" + "é" * 511 + "\n" in failed.stderr, failed.stderr
            assert not endpoint.exists()
    print(f"{args.scenario}: blocked renderer, native deadline, EOF and private endpoint cleanup passed")


def main():
    parser = argparse.ArgumentParser()
    for name in ("agent", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--scenario", choices=("session", "mcp-watchdog", "cli-watchdog"), default="session")
    args = parser.parse_args()
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

        # A human-owned window is already running before either agent attaches. Native grants remain the owner's.
        endpoint = pathlib.Path(temporary) / "shared-window"
        # Keep Chromium's temporary Unix socket paths below sockaddr_un's limit, independently
        # of the checkout/work path length. The agent still creates its own private child profile.
        with tempfile.TemporaryDirectory(prefix="wf-profile-", ignore_cleanup_errors=True) as profile_root, (pathlib.Path(temporary) / "shared.log").open("w") as log:
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
