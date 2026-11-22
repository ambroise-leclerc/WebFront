"""Optional real-window interoperability regression, using the official MCP Python SDK 2.3.0 over Streamable HTTP.

An application that enabled automation (automation_host) serves MCP itself (ADR-025); the SDK connects to its URL with the
Bearer token of its private mcp.json. Install the pinned SDK in a separate test venv; neither configure nor this test
downloads packages.
"""
import argparse
import asyncio
import importlib.metadata
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

from mcp import ClientSession
from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from AutomationClient import Host, check_withdrawn, install, interrupt_status  # noqa: E402


async def exercise(configuration):
    server = json.loads(pathlib.Path(configuration).read_text())["mcpServers"]["webfront"]
    async with create_mcp_http_client(headers=server["headers"]) as http, \
            streamable_http_client(server["url"], http_client=http) as streams:
        reader, writer = streams[0], streams[1]
        async with ClientSession(reader, writer) as client:
            initialized = await client.initialize()
            assert initialized.protocol_version == "2025-11-25"
            assert "webfront.session" in (initialized.instructions or "")
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
    for name in ("host", "driver", "environment", "provision", "registry", "fixture", "work"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    assert importlib.metadata.version("mcp") == "2.3.0", "Use the pinned optional SDK mcp==2.3.0"
    pathlib.Path(args.work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix="sdk-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project"
        shutil.copytree(args.fixture, project)
        install(args, project)
        with (pathlib.Path(args.work) / "sdk-interop.log").open("w") as log:
            host = Host(args, project, root, "sdk", log, ["--grants", "build"])
            try:
                record = host.record()
                first = await asyncio.wait_for(exercise(record["configuration"]), 90)
                # A second SDK client after the first one terminated its MCP session: the window and its session stay.
                assert host.process.poll() is None, "Ending an SDK session closed the application's window"
                assert await asyncio.wait_for(exercise(record["configuration"]), 90) == first
                host.interrupt()
                assert host.wait(timeout=30) == interrupt_status(), host.logs()
                check_withdrawn(record, int(record["mcp"].rsplit(":", 1)[1].split("/")[0]))
            finally:
                host.close()
    print("Official MCP Python SDK 2.3.0: initialization, catalogue, refusal, build and reconnection over Streamable HTTP passed")


if __name__ == "__main__":
    asyncio.run(main())
