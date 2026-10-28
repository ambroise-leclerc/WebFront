"""Real stdio MCP client driving an authorized developer window, with no third-party Python modules."""
import argparse
import json
import pathlib
import queue
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


class Client:
    def __init__(self, command, stderr, timeout=30):
        self.timeout = timeout  # seconds for the session to become ready and for one operation to finish
        self.child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr, text=True, encoding="utf-8")
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


def main():
    parser = argparse.ArgumentParser()
    for name in ("agent", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="session-") as temporary:
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
                assert client.wait(run)["state"] == "cancelled"
                client.run("test", {"pattern": "passes"})
            finally:
                client.close()
            assert client.child.returncode == 0, (client.child.returncode, pathlib.Path(log.name).read_text())

        # A human-owned window is already running before either agent attaches. Native grants remain the owner's.
        endpoint = pathlib.Path(temporary) / "shared-window"
        with (pathlib.Path(temporary) / "shared.log").open("w") as log:
            owner = subprocess.Popen([args.agent, *command, "--serve", "--listen", str(endpoint),
                                      "--allow", "build", "--allow", "test", "--timeout", "20"],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=log, text=True)
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
                    assert first.wait(run)["state"] == "cancelled"
                finally:
                    first.close()
                    second.close()
                assert owner.poll() is None, "Detaching an agent closed the human's window"
                cli = subprocess.run([args.agent, "--attach", str(endpoint), "--operation", "build"],
                                     capture_output=True, text=True, timeout=30)
                assert cli.returncode == 0 and json.loads(cli.stdout)["state"] == "succeeded", cli.stdout + cli.stderr
                shutdown = subprocess.run([args.agent, "--attach", str(endpoint), "--shutdown"],
                                          capture_output=True, text=True, timeout=10)
                assert shutdown.returncode == 0, shutdown.stderr
                assert owner.wait(timeout=10) == 0
                assert not endpoint.exists(), "The owner left its attachment capability behind"
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait()

        # A short operation deadline must not expire during a deliberately slow bootstrap.
        delayed = pathlib.Path(temporary) / "delayed-environment.tgz"
        with tarfile.open(args.environment, "r:gz") as archive, tarfile.open(delayed, "w:gz") as output:
            for member in archive.getmembers():
                data = archive.extractfile(member).read() if member.isfile() else None
                if member.name == "package/runtime.mjs":
                    data = b"await new Promise(resolve => setTimeout(resolve,7000));\n" + data
                    member.size = len(data)
                output.addfile(member, io.BytesIO(data) if data is not None else None)
        delayed_command = list(command)
        delayed_command[delayed_command.index("--environment") + 1] = str(delayed)
        delayed_command[delayed_command.index("--integrity") + 1] = "sha512-" + base64.b64encode(hashlib.sha512(delayed.read_bytes()).digest()).decode()
        with (pathlib.Path(temporary) / "hang.log").open("w") as log:
            client = Client([args.agent, *delayed_command, "--mcp", "--startup-timeout", "30", "--timeout", "5", "--allow", "test"], log)
            try:
                client.initialize()
                client.start("test", {"pattern": "blocks the renderer"})
                assert client.child.wait(timeout=15) == 71
                assert (project / "renderer.started").is_file(), "The native deadline expired before the blocked spec actually started"
            finally:
                client.close()
        (project / "renderer.started").unlink()
        cli = subprocess.run([args.agent, *delayed_command, "--startup-timeout", "30", "--timeout", "5",
                              "--allow", "test", "--operation", "test", "--pattern", "blocks the renderer"],
                             capture_output=True, text=True, timeout=45)
        assert cli.returncode == 71, cli.stdout + cli.stderr
        assert (project / "renderer.started").is_file(), "The CLI deadline expired before the blocked spec started"
    print("MCP real-window regression passed: diagnostics/correction/check/build/test/preview/actions/cancellation/native watchdog/authenticated shared-window attachment")


if __name__ == "__main__":
    main()
