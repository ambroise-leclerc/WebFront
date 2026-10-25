"""Transfer a chosen commit and an offline dependency kit, then run Linux lanes over SSH."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import signal
import subprocess
import tarfile
import tempfile
import threading
import time
import uuid

HERE = Path(__file__).resolve().parent
STOP = threading.Event()


def command(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def safeName(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or not path.parts:
        raise ValueError(f"Unsafe archive path: {name}")
    return path


def extract(archive, destination):
    # No links, devices or duplicate paths: never let an archive write outside its run directory.
    with tarfile.open(archive) as source:
        names = set()
        for member in source:
            name = safeName(member.name).as_posix()
            if name in names or not (member.isfile() or member.isdir()):
                raise ValueError(f"Unsupported archive member: {member.name}")
            names.add(name)
            target = destination / name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as src, open(target, "xb") as dst:
                    import shutil
                    shutil.copyfileobj(src, dst)
                target.chmod(member.mode & 0o777)


def inventory(root):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Dereference dependency symlinks before packing: {path}")
        if ".git" in path.relative_to(root).parts:
            raise ValueError(f"Export source trees without Git metadata: {path}")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
        elif not path.is_dir():
            raise ValueError(f"Unsupported dependency file: {path}")
    return result


def verifyKit(root):
    manifest = json.loads((root / "kit.json").read_text())
    actual = inventory(root)
    del actual["kit.json"]
    if actual != manifest["files"]:
        raise ValueError("Offline kit integrity mismatch (missing, changed or extra files)")
    if not (root / "preload.cmake").is_file():
        raise ValueError("Offline kit needs preload.cmake")
    for name, image in manifest["images"].items():
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", image["id"]):
            raise ValueError(f"Image {name} needs an immutable Docker image ID")
        safeName(image["archive"])
        if image["archive"] not in actual:
            raise ValueError(f"Image archive missing: {name}")
    return manifest


def pack(args):
    root = args.dependencies.resolve()
    if args.output.resolve().is_relative_to(root):
        raise ValueError("The output archive must be outside the dependency directory")
    config = json.loads((root / "provision.json").read_text())
    if not config.get("sources") or not config.get("images"):
        raise ValueError("provision.json must describe images and source provenance")
    # Require provenance for every top-level tree (archives retain their upstream checksums).
    for path in root.iterdir():
        if path.name not in {"provision.json", "kit.json"} and path.name not in config["sources"]:
            raise ValueError(f"Missing source URL/revision/integrity for {path.name}")
    for name, origin in config["sources"].items():
        if not isinstance(origin, dict) or not origin.get("origin") or not origin.get("integrity"):
            raise ValueError(f"Incomplete source provenance: {name}")
    files = inventory(root)
    files.pop("kit.json", None)
    manifest = dict(config, files=files, schema=1)
    # Generate metadata in the archive without modifying the prepared dependency directory.
    import io
    with tarfile.open(args.output, "w:gz") as archive:
        for name in files:
            archive.add(root / name, arcname=name, recursive=False)
        data = json.dumps(manifest, indent=2).encode()
        info = tarfile.TarInfo("kit.json")
        info.size = len(data)
        archive.addfile(info, io.BytesIO(data))
    print(f"{digest(args.output)}  {args.output}")
    return 0


def selectedLanes(matrix, machines, lanes):
    def names(value):
        return set(value.split(",")) if value else set()
    machineNames, laneNames = names(machines), names(lanes)
    knownMachines = {lane["machine"] for lane in matrix["lanes"]}
    knownLanes = {lane["id"] for lane in matrix["lanes"]}
    if machineNames - knownMachines or laneNames - knownLanes:
        raise ValueError("Unknown machine or lane selection")
    result = [lane for lane in matrix["lanes"] if
              (not machineNames or lane["machine"] in machineNames) and
              (not laneNames or lane["id"] in laneNames)]
    if not result:
        raise ValueError("No lanes selected")
    return result


def exitCode(results, requireAll):
    if not any(row["status"] == "passed" for row in results):
        return 1
    if any(row["status"] in {"failed", "interrupted", "timeout"} for row in results):
        return 1
    return int(requireAll and any(row["status"] != "passed" for row in results))


def ssh(machine, script):
    return ["ssh", "-oBatchMode=yes", "-oConnectTimeout=5", "-oServerAliveInterval=10",
            "-oServerAliveCountMax=2", machine, script]


def remote(machine, args):
    # All values are shell quoted once, at the SSH command-string boundary.
    return ssh(machine, shlex.join(args))


def runProcess(args, log, timeout, stdin=None):
    started = time.monotonic()
    process = subprocess.Popen(args, stdin=stdin or subprocess.DEVNULL, stdout=log,
                               stderr=subprocess.STDOUT)
    try:
        while process.poll() is None:
            if STOP.wait(0.1):
                raise InterruptedError("Interrupted")
            if time.monotonic() - started > timeout:
                raise TimeoutError(f"Timeout after {timeout}s")
        if process.returncode:
            raise subprocess.CalledProcessError(process.returncode, args)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


# Bootstraps only into a unique directory under the dedicated account's CI root.
# The worker and its safe extractor are transmitted independently of the tested commit.
BOOTSTRAP = '''import pathlib,sys,tarfile
root=pathlib.Path.home()/".webfront-local-ci"/"runs"/sys.argv[1]
root.mkdir(parents=True,exist_ok=False)
with tarfile.open(fileobj=sys.stdin.buffer,mode="r|*") as archive:
 for item in archive:
  p=pathlib.PurePosixPath(item.name)
  if p.is_absolute() or ".." in p.parts or "\\\\" in item.name or not item.isfile(): raise ValueError("Unsafe payload")
  target=root/p
  target.parent.mkdir(parents=True,exist_ok=True)
  with archive.extractfile(item) as src,open(target,"xb") as dst:
   import shutil
   shutil.copyfileobj(src,dst)
print(root)
'''


def execute(args):
    STOP.clear()
    repository = Path(command(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                              text=True).stdout.strip())
    if not args.allow_dirty and command(["git", "status", "--porcelain"], cwd=repository,
                                       capture_output=True, text=True).stdout:
        raise ValueError("Working tree is modified; commit changes or use --allow-dirty (tests the commit only)")
    sha = command(["git", "rev-parse", "--verify", args.commit + "^{commit}"], cwd=repository,
                  capture_output=True, text=True).stdout.strip()
    matrix = json.loads(command(["git", "show", sha + ":cmake/local-ci/lanes.json"], cwd=repository,
                                capture_output=True, text=True).stdout)
    lanes = selectedLanes(matrix, args.machines, args.lanes)
    runId = sha[:12] + "-" + uuid.uuid4().hex[:12]
    output = repository / "build-local-ci" / sha / runId
    output.mkdir(parents=True)
    rows = []
    previous = signal.signal(signal.SIGINT, lambda *_: STOP.set())
    try:
        with tempfile.TemporaryDirectory(prefix="webfront-local-ci-") as scratch:
            scratch = Path(scratch)
            kit = None
            if args.kit:
                if digest(args.kit) != args.kit_sha256:
                    raise ValueError("Offline kit SHA-256 mismatch")
                kit = args.kit.resolve()
            bundle = scratch / "commit.bundle"
            # A temporary private ref lets git bundle include detached and unpushed commits.
            ref = "refs/local-ci/" + runId
            command(["git", "update-ref", ref, sha], cwd=repository)
            try:
                command(["git", "bundle", "create", str(bundle), ref], cwd=repository)
            finally:
                command(["git", "update-ref", "-d", ref], cwd=repository)
            payload = scratch / "payload.tar"
            selectedMatrix = scratch / "lanes.json"
            selectedMatrix.write_text(json.dumps(matrix))
            with tarfile.open(payload, "w") as archive:
                for path, name in [(bundle, "commit.bundle"), (HERE / "worker.py", "worker.py"),
                                   (HERE / "orchestrator.py", "orchestrator.py"),
                                   (selectedMatrix, "lanes.json")]:
                    archive.add(path, arcname=name)
                if kit:
                    archive.add(kit, arcname="kit.tgz")
            machines = {lane["machine"] for lane in lanes}
            ready = {}
            def prepare(machine):
                with open(output / (machine + "-prepare.log"), "wb") as log:
                    try:
                        runProcess(ssh(machine, "echo webfront-local-ci"), log, 10)
                    except InterruptedError:
                        return "interrupted"
                    except (subprocess.SubprocessError, TimeoutError, OSError):
                        return "unreachable"
                    if machine != "RyzenLinux":
                        return "skipped (native session support pending step 3)"
                    if not kit:
                        log.write(b"Linux lanes require --kit and --kit-sha256; prepare an offline kit first.\n")
                        return "failed"
                    try:
                        with open(payload, "rb") as stream:
                            runProcess(remote(machine, ["python3", "-c", BOOTSTRAP, runId]), log,
                                       args.transfer_timeout, stream)
                        runProcess(remote(machine, ["python3", f".webfront-local-ci/runs/{runId}/worker.py",
                                                   "prepare", "--kit-sha256", args.kit_sha256]), log,
                                   args.transfer_timeout)
                        return "ready"
                    except InterruptedError:
                        return "interrupted"
                    except TimeoutError:
                        return "timeout"
                    except (subprocess.SubprocessError, OSError):
                        return "failed"
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(machines)) as pool:
                pending = {pool.submit(prepare, machine): machine for machine in machines}
                for task in concurrent.futures.as_completed(pending):
                    ready[pending[task]] = task.result()
            def laneTask(lane):
                start = time.monotonic()
                machine, laneId = lane["machine"], lane["id"]
                status = ready[machine]
                with open(output / (laneId + ".log"), "wb") as log:
                    if status == "ready":
                        try:
                            runProcess(remote(machine, ["python3", f".webfront-local-ci/runs/{runId}/worker.py",
                                "lane", "--lane", laneId, "--sha", sha, "--jobs", str(args.jobs),
                                "--timeout", str(args.timeout)] + (["--keep"] if args.keep else [])),
                                log, args.timeout + 60)
                            status = "passed"
                        except InterruptedError:
                            status = "interrupted"
                        except TimeoutError:
                            status = "timeout"
                        except subprocess.CalledProcessError as error:
                            status = "timeout" if error.returncode == 124 else "failed"
                        except (subprocess.SubprocessError, OSError):
                            status = "failed"
                        if status in {"interrupted", "timeout", "failed"}:
                            # A second SSH channel stops the container even if the execution channel was lost.
                            try:
                                command(remote(machine, ["python3", f".webfront-local-ci/runs/{runId}/worker.py",
                                                         "stop", "--lane", laneId]), stdout=log,
                                        stderr=subprocess.STDOUT, timeout=15)
                            except (subprocess.SubprocessError, OSError):
                                log.write(b"Remote cancellation could not be confirmed; inspect the container.\n")
                        try:
                            with open(output / (laneId + "-artifacts.tar"), "wb") as artifacts:
                                command(remote(machine, ["python3", f".webfront-local-ci/runs/{runId}/worker.py",
                                                         "artifacts", "--lane", laneId]), stdout=artifacts,
                                        stderr=log, timeout=60)
                        except (subprocess.SubprocessError, OSError):
                            log.write(b"Artifact collection failed.\n")
                            if status == "passed":
                                status = "failed"
                    else:
                        log.write((status + "; see machine preparation log\n").encode())
                row = {"lane": laneId, "machine": machine, "status": status,
                       "seconds": round(time.monotonic() - start, 1)}
                print(f"{laneId:44} {status:58} {row['seconds']:7.1f}s {machine}", flush=True)
                return row
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel_lanes) as pool:
                rows = list(pool.map(laneTask, lanes))
            # Retain only explicit --keep runs; caches are outside the per-run directory.
            if not args.keep:
                for machine, status in ready.items():
                    if machine == "RyzenLinux" and status != "unreachable":
                        with open(output / (machine + "-cleanup.log"), "wb") as log:
                            try:
                                command(remote(machine, ["python3", f".webfront-local-ci/runs/{runId}/worker.py",
                                                         "cleanup"]), stdout=log, stderr=subprocess.STDOUT, timeout=30)
                            except (subprocess.SubprocessError, OSError):
                                print(f"Cleanup not confirmed on {machine}; run {runId} retained")
    finally:
        signal.signal(signal.SIGINT, previous)
        (output / "results.json").write_text(json.dumps({"sha": sha, "run": runId, "results": rows}, indent=2))
    complete = len(rows) == len(matrix["lanes"]) and all(r["status"] == "passed" for r in rows)
    print(f"{'Complete' if complete else 'INCOMPLETE'} matrix. Logs: {output}")
    return exitCode(rows, args.require_all) if not STOP.is_set() else 130


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    packParser = sub.add_parser("pack", help="Seal a prepared offline dependency/image directory")
    packParser.add_argument("--dependencies", type=Path, required=True)
    packParser.add_argument("--output", type=Path, required=True)
    run = sub.add_parser("run", help="Test a commit (default: all lanes in the selected commit)")
    run.add_argument("--commit", default="HEAD")
    run.add_argument("--allow-dirty", action="store_true")
    run.add_argument("--machines")
    run.add_argument("--lanes")
    run.add_argument("--require-all", action="store_true")
    run.add_argument("--keep", action="store_true")
    run.add_argument("--kit", type=Path)
    run.add_argument("--kit-sha256")
    run.add_argument("--parallel-lanes", type=int, default=2)
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--timeout", type=int, default=3600)
    run.add_argument("--transfer-timeout", type=int, default=1800)
    args = parser.parse_args(argv)
    try:
        if args.action == "pack":
            return pack(args)
        if min(args.parallel_lanes, args.jobs, args.timeout, args.transfer_timeout) < 1:
            raise ValueError("Concurrency and timeout values must be positive")
        if bool(args.kit) != bool(args.kit_sha256):
            raise ValueError("--kit and --kit-sha256 must be supplied together")
        return execute(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"local-ci: {error}", file=__import__('sys').stderr)
        return 1
