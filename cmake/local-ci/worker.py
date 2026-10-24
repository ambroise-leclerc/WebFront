"""Remote Linux worker and in-container lane executor. Never downloads dependencies."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
from orchestrator import command, digest, extract, verifyKit, runProcess, STOP

ROOT = Path(__file__).resolve().parent

CLEAN_WORKSPACE = """from pathlib import Path
import shutil
for child in Path('/workspace').iterdir():
    if child.is_dir() and not child.is_symlink():
        shutil.rmtree(child)
    else:
        child.unlink()
"""


def cleanWorkspace(workspace, image, laneId):
    # Browser profiles can be 0700, owned by the container's mapped UID rather
    # than the host account. Reuse the test UID to remove them without extra caps.
    name = containerName(laneId) + "-cleanup"
    try:
        command(["docker", "run", "--rm", "--pull=never", "--network=none", "--name", name,
                 "--user", f"{os.getuid()}:{os.getgid()}", "--group-add=0", "--cap-drop=ALL",
                 "--security-opt=no-new-privileges",
                 "--mount", f"type=bind,src={workspace},dst=/workspace",
                 image, "python3", "-c", CLEAN_WORKSPACE], timeout=60)
    finally:
        subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=15)
    workspace.rmdir()


def shareDirectory(path):
    # In rootless Docker, container group 0 maps to the dedicated host account's group.
    # Setgid + group writes let the non-root container and host collect/clean the same tree.
    for directory, _, files in os.walk(path):
        target = Path(directory)
        target.chmod(target.stat().st_mode | 0o2070)
        for name in files:
            target = Path(directory) / name
            if not target.is_symlink():
                target.chmod(target.stat().st_mode | 0o060)


def containerName(lane):
    return "wf-" + ROOT.name + "-" + lane


def prepare(expected):
    if os.getuid() == 0:
        raise ValueError("Use a dedicated non-root SSH account and rootless Docker")
    security = command(["docker", "info", "--format", "{{json .SecurityOptions}}"],
                       capture_output=True, text=True).stdout
    if "name=rootless" not in json.loads(security):
        raise ValueError("The dedicated account must use rootless Docker")
    if digest(ROOT / "kit.tgz") != expected:
        raise ValueError("Transferred kit SHA-256 mismatch")
    dependencies = ROOT / "dependencies"
    dependencies.mkdir()
    extract(ROOT / "kit.tgz", dependencies)
    manifest = verifyKit(dependencies)
    cache = ROOT.parent.parent / "cache/cpm/cpm"
    cache.mkdir(parents=True, exist_ok=True)
    for module in (dependencies / "cpm").glob("CPM_*.cmake"):
        temporary = cache / (module.name + "." + ROOT.name)
        shutil.copyfile(module, temporary)
        temporary.replace(cache / module.name)
    for image in manifest["images"].values():
        command(["docker", "load", "--input", str(dependencies / image["archive"])])
        command(["docker", "image", "inspect", image["id"]], stdout=subprocess.DEVNULL)
    # --mirror preserves refs/local-ci/*, including a detached commit with no branch.
    command(["git", "clone", "--mirror", str(ROOT / "commit.bundle"), str(ROOT / "repo.git")])


def ctestCommand(lane, build):
    ctest = lane["ctest"]
    args = ["ctest", "--test-dir", str(build)] + ctest["args"]
    for label in ctest["labels"]:
        args += ["-L", label]
    for label in ctest["exclude_labels"]:
        args += ["-LE", label]
    return args


def buildCommands(lane, jobs, workspace="/workspace"):
    """The same lane data used by the drift checker supplies every build/test command."""
    # Commands run in a Linux container even when planned/tested on Windows.
    workspace = PurePosixPath(workspace)
    commands = []
    variants = lane.get("configure_variants", [[]])
    for index, variant in enumerate(variants):
        build = workspace / f"build-local-{index}"
        options = lane["common_options"] + lane["options"] + variant
        options = [option.replace("@workspace@", str(workspace)) for option in options]
        configure = ["cmake", "-S", str(workspace), "-B", str(build), "-G", "Ninja",
                     "-C", "/deps/preload.cmake", "-DCMAKE_BUILD_TYPE=" + lane["build_type"]]
        if lane.get("toolchain_file"):
            configure += ["-DCMAKE_TOOLCHAIN_FILE=" + str(workspace / lane["toolchain_file"])]
        configure += options
        if "-DWEBFRONT_EMBED_CEF=ON" in options:
            configure += ["-DFETCHCONTENT_SOURCE_DIR_CEF_BINARIES=" + str(build / "_deps/cef_binaries-src")]
        # Local differences are intentional: CPM/ccache persistence, local JUnit paths.
        configure += ["-DCPM_SOURCE_CACHE=/cache/cpm", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                      "-DWEBFRONT_BOOTSTRAP_DIR=/deps/bootstrap", "-DWEBFRONT_REGISTRY_DIR=/deps/registry",
                      "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
        commands.append(configure)
        buildArgs = ["cmake", "--build", str(build), "--parallel", str(jobs)]
        if lane["targets"]:
            buildArgs += ["--target"] + lane["targets"]
        commands.append(buildArgs)
        if lane.get("catch2_direct"):
            for invocation in lane["catch2_direct"]:
                repeat = int(invocation[-1][1:]) if invocation[-1].startswith("x") else 1
                arguments = invocation[:-1] if repeat != 1 else invocation
                for _ in range(repeat):
                    commands.append([str(build / "test/unit/tests")] + arguments)
        else:
            commands.append(ctestCommand(lane, build) + ["--output-junit", str(build / "test-results.xml")])
        if lane.get("modes") and index == 0:
            commands.append(["cmake", "--install", str(build / "mddlog"), "--prefix",
                             str(workspace / "mddlog-install")])
    build = workspace / "build-local-0"
    for decoder in lane.get("fuzz_decoders", []):
        commands.append([str(build / f"test/fuzz/fuzz_{decoder}"),
                         str(build / f"test/fuzz/corpus/{decoder}"),
                         f"-max_total_time={lane['fuzz_seconds']}", "-timeout=5", "-max_len=65536",
                         "-rss_limit_mb=1024", f"-artifact_prefix={build}/test/fuzz/artifacts/{decoder}-"])
    if "lcov-coverage" in lane.get("extra_steps", []):
        coverage = str(build / "coverage.info")
        commands += [
            ["lcov", "--ignore-errors", "inconsistent,inconsistent", "--gcov-tool", "gcov-16", "--capture",
             "--directory", str(build), "--output-file", coverage],
            ["lcov", "--ignore-errors", "inconsistent,inconsistent", "--remove", coverage,
             "/usr/*", "*/examples/*", "--output-file", coverage],
            ["lcov", "--list", coverage]]
    return commands


def executeInside(lane, jobs):
    os.umask(0o002)
    cmakeVersion = command(["cmake", "--version"], capture_output=True, text=True).stdout
    print(cmakeVersion, flush=True)
    if cmakeVersion.splitlines()[0] != "cmake version " + lane["cmake_version"]:
        raise ValueError("CMake version does not match the lane")
    family, version = lane["compiler"]["family"], lane["compiler"]["version"]
    if family == "clang":
        os.environ.update(CC="clang-" + version, CXX="clang++-" + version)
    elif version == "16":
        os.environ.update(CC="gcc-16", CXX="g++-16")
    else:
        os.environ.update(CC="gcc", CXX="g++")
    command([os.environ["CXX"], "--version"])
    command(["ccache", "--version"])
    if family == "clang" or version == "16":
        actual = command([os.environ["CXX"], "-dumpversion"], capture_output=True, text=True).stdout.strip()
        if actual.split(".")[0] != version:
            raise ValueError("Compiler major version does not match the lane")
    # Apply the WebKit sandbox exception only to test commands, as in the workflow.
    env = lane.get("env", {})
    os.environ.update({key: value for key, value in env.items() if key != "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"})
    os.environ.update(CCACHE_DIR="/cache/ccache", CCACHE_BASEDIR="/workspace")
    if "-DWEBFRONT_EMBED_CEF=ON" in lane["options"]:
        # CEFSupport copies runtime resources into this SDK tree during the build.
        # Keep that writable tree private to the lane, at the path CEFSupport uses.
        shutil.copytree("/deps/cef-binaries", "/workspace/build-local-0/_deps/cef_binaries-src")
        shareDirectory(Path("/workspace/build-local-0/_deps/cef_binaries-src"))
    for invocation in buildCommands(lane, jobs):
        if lane.get("fuzz_decoders"):
            Path("/workspace/build-local-0/test/fuzz/artifacts").mkdir(parents=True, exist_ok=True)
        print("+", repr(invocation), flush=True)
        testEnv = dict(os.environ)
        if invocation[0] == "ctest":
            testEnv.update(env)
        command(invocation, cwd="/workspace", env=testEnv)
    command(["ccache", "--show-stats"])


def runLane(args, matrix):
    lane = next(row for row in matrix["lanes"] if row["id"] == args.lane)
    if not lane["runtime"].startswith("container:"):
        raise ValueError("Native lanes are not implemented in delivery step 2")
    for key, expected in lane.get("host_requirements", {}).get("sysctl", {}).items():
        actual = command(["sysctl", "-n", key], capture_output=True, text=True).stdout.strip()
        if actual != expected:
            raise ValueError(f"Host requires {key}={expected}; found {actual}. Ask its owner to configure it.")
    manifest = json.loads((ROOT / "dependencies/kit.json").read_text())
    image = manifest["images"][lane["runtime"].split(":", 1)[1]]["id"]
    workspace = ROOT / lane["id"]
    # A standalone .git works at /workspace in the container; linked worktrees
    # would retain absolute host paths in their .git files and object alternates.
    workspace.mkdir()
    command(["git", "clone", "--mirror", "--no-hardlinks", str(ROOT / "repo.git"), str(workspace / ".git")])
    command(["git", "-C", str(workspace), "config", "core.bare", "false"])
    command(["git", "-C", str(workspace), "checkout", "--detach", args.sha])
    if lane["workflow"] == "MddlogIntegration.yml":
        if manifest.get("mddlog_ref") != matrix["mddlog"]["ref"]:
            raise ValueError("Offline kit mddlog revision does not match lanes.json")
        shutil.copytree(ROOT / "dependencies/mddlog", workspace / "dependencies/mddlog")
    cache = ROOT.parent.parent / "cache"
    (cache / "cpm").mkdir(parents=True, exist_ok=True)
    (cache / "ccache").mkdir(parents=True, exist_ok=True)
    shareDirectory(workspace)
    # Do not traverse shared caches while another lane is writing them.
    for directory in (cache, cache / "cpm", cache / "ccache"):
        directory.chmod(directory.stat().st_mode | 0o2070)
    invocation = ["docker", "run", "--rm", "--pull=never", "--name", containerName(args.lane),
                  "--network=none", "--shm-size=" + lane.get("container", {}).get("shm_size", "1g"),
                  "--user", f"{os.getuid()}:{os.getgid()}", "--group-add=0", "--cap-drop=ALL",
                  "--security-opt=no-new-privileges", "--init",
                  "--mount", f"type=bind,src={workspace},dst=/workspace",
                  "--mount", f"type=bind,src={ROOT / 'dependencies'},dst=/deps,readonly",
                  "--mount", f"type=bind,src={ROOT},dst=/runner,readonly",
                  "--mount", f"type=bind,src={cache},dst=/cache",
                  "--env", "GIT_CONFIG_COUNT=1", "--env", "GIT_CONFIG_KEY_0=safe.directory",
                  "--env", "GIT_CONFIG_VALUE_0=/workspace",
                  "--env", "HOME=/tmp", "--workdir", "/workspace", image,
                  "python3", "/runner/worker.py", "inside", "--lane", args.lane, "--jobs", str(args.jobs)]
    try:
        runProcess(invocation, sys.stdout, args.timeout)
    finally:
        # Stop is idempotent; a timeout or disconnected SSH must not leave a running container.
        subprocess.run(["docker", "rm", "-f", containerName(args.lane)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=15)
        # Save diagnostics before removing the worktree, including failed configurations.
        with tarfile.open(ROOT / (args.lane + "-artifacts.tar"), "w") as archive:
            for pattern in ("**/coverage.info", "**/test-results.xml", "**/LastTest.log",
                            "**/CMakeConfigureLog.yaml", "**/artifacts/*"):
                for path in workspace.glob(pattern):
                    if path.is_file() and not path.is_symlink():
                        archive.add(path, arcname=path.relative_to(workspace), recursive=False)
        if not args.keep:
            cleanWorkspace(workspace, image, args.lane)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["prepare", "lane", "inside", "stop", "cleanup", "artifacts"])
    parser.add_argument("--kit-sha256")
    parser.add_argument("--lane")
    parser.add_argument("--sha")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())
    signal.signal(signal.SIGHUP, lambda *_: STOP.set())
    matrix = json.loads((ROOT / "lanes.json").read_text())
    try:
        if args.action == "prepare":
            prepare(args.kit_sha256)
        elif args.action == "lane":
            runLane(args, matrix)
        elif args.action == "inside":
            executeInside(next(row for row in matrix["lanes"] if row["id"] == args.lane), args.jobs)
        elif args.action == "stop":
            command(["docker", "rm", "-f", containerName(args.lane)])
        elif args.action == "artifacts":
            with open(ROOT / (args.lane + "-artifacts.tar"), "rb") as source:
                shutil.copyfileobj(source, sys.stdout.buffer)
        elif args.action == "cleanup":
            if ROOT.parent.name != "runs" or ROOT.parent.parent.name != ".webfront-local-ci":
                raise ValueError("Refusing cleanup outside a local CI run")
            shutil.rmtree(ROOT)
        return 0
    except TimeoutError as error:
        print(f"local-ci worker: {error}", file=sys.stderr, flush=True)
        return 124
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"local-ci worker: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
