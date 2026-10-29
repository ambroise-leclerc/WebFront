"""Optional real-window interoperability regression, using the official MCP Python SDK 2.3.0.

Install the pinned SDK in a separate test venv; neither configure nor this test downloads packages.
"""
import argparse
import asyncio
import importlib.metadata
import json
import os
import pathlib
import shutil
import subprocess
import tempfile

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def exercise(command, log):
    parameters = StdioServerParameters(command=command[0], args=command[1:], env=os.environ.copy())
    async with stdio_client(parameters, errlog=log) as (reader, writer):
        async with ClientSession(reader, writer) as client:
            initialized = await client.initialize()
            assert initialized.protocol_version == "2025-11-25"
            assert {tool.name for tool in (await client.list_tools()).tools} == {
                "webfront.session", "webfront.start", "webfront.result", "webfront.cancel"}
            for _ in range(600):
                described = await client.call_tool("webfront.session", {})
                assert not described.is_error, described
                state = described.structured_content
                if state["state"] == "ready":
                    break
                assert state["state"] != "closed", state
                await asyncio.sleep(0.05)
            else:
                raise TimeoutError("SDK session did not become ready")
            session = state["session"]
            denied = await client.call_tool("webfront.start", {"session": session, "operation": "test"})
            assert denied.is_error and denied.structured_content["code"] in ("invalid_arguments", "not_authorized")
            started = await client.call_tool("webfront.start", {"session": session, "operation": "build"})
            assert not started.is_error, started
            run = started.structured_content["runId"]
            for _ in range(600):
                result = await client.call_tool("webfront.result", {"session": session, "runId": run})
                assert not result.is_error, result
                state = result.structured_content
                if state["state"] not in ("queued", "running"):
                    break
                await asyncio.sleep(0.05)
            assert state["state"] == "succeeded", state
            assert state["result"]["exitCode"] == 0
            assert state["output"] == state["result"]["output"]
            return session


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("agent", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    assert importlib.metadata.version("mcp") == "2.3.0", "Use the pinned optional SDK mcp==2.3.0"
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="sdk-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        integrity = json.loads(pathlib.Path(args.provision).read_text())["integrity"]
        common = ["--project", str(project), "--environment", args.environment, "--integrity", integrity, "--registry-dir", args.registry]
        installed = subprocess.run([args.driver, *common, "--", "install"], capture_output=True, text=True, timeout=120)
        assert installed.returncode == 0, installed.stdout + installed.stderr
        with (pathlib.Path(args.work) / "sdk-interop.log").open("w") as log:
            await asyncio.wait_for(exercise([args.agent, *common, "--mcp", "--allow", "build"], log), 90)
            endpoint = root / "endpoint"
            owner = subprocess.Popen([args.agent, *common, "--serve", "--listen", str(endpoint), "--allow", "build"],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log)
            try:
                for _ in range(600):
                    assert owner.poll() is None, log.name
                    if (endpoint / "endpoint.json").is_file():
                        break
                    await asyncio.sleep(0.05)
                assert (endpoint / "endpoint.json").is_file()
                await asyncio.wait_for(exercise([args.agent, "--attach", str(endpoint), "--mcp"], log), 90)
                assert owner.poll() is None, "SDK detachment closed the human-owned window"
                stopped = subprocess.run([args.agent, "--attach", str(endpoint), "--shutdown"], capture_output=True, text=True, timeout=15)
                assert stopped.returncode == 0, stopped.stderr
                assert owner.wait(timeout=15) == 0
                assert not endpoint.exists()
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait()
    print("Official MCP Python SDK 2.3.0: owned window and authenticated shared-window attachment passed")


if __name__ == "__main__":
    asyncio.run(main())
