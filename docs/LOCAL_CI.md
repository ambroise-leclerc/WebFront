# Local CI matrix

Story #346: run the whole CI matrix of WebFront from a development machine, over SSH, on three machines of the local network
(`RyzenLinux`, `CurmudgeonWindows`, `Mac`), to validate a commit before pushing it. The machines stay under the sole control of
their owner: no self-hosted GitHub runner, no inbound connection from GitHub, no GitHub secret on the machines. GitHub CI remains the
reference for pull requests.

## State of the delivery

The story is delivered in three verifiable steps; a partial step does not close it.

| Step | Content | State |
| --- | --- | --- |
| 1 | The ten lanes in `cmake/local-ci/lanes.json` and the offline drift check `LocalCiLanesMatchWorkflows` | delivered |
| 2 | Commit transport, offline-kit verification, Linux containers and the orchestrator | implemented; real-host/image qualification pending |
| 3 | Native Windows and macOS sessions, result collection, full qualification and timings | not started |

## The lanes

`BuildAndTest.yml` expands to seven lanes and `MddlogIntegration.yml` to three. The workflows' trigger conditions (push, schedule,
manual dispatch) do not exempt the local matrix from the extended lanes: a full run selects all ten.

| Lane | GitHub job | Machine | Toolchain | CMake |
| --- | --- | --- | --- | --- |
| `desktop-linux-gcc-webkitgtk` | `desktop` / `linux-gcc-webkitgtk` | `RyzenLinux` (container) | GCC 16, Debug, coverage, WebKitGTK | 4.4.4 |
| `desktop-windows-msvc-webview2` | `desktop` / `windows-msvc-webview2` | `CurmudgeonWindows` (native) | MSVC, Release, WebView2 | 4.4.4 |
| `desktop-macos-clang-wkwebview` | `desktop` / `macos-clang-wkwebview` | `Mac` (native) | AppleClang (latest Xcode), Release, WKWebView | 4.4.4 |
| `sanitizers-address-undefined` | `sanitizers` / `address-undefined` | `RyzenLinux` (container) | Clang 23, ASan+UBSan, fuzzing of four decoders | 4.4.4 |
| `sanitizers-thread` | `sanitizers` / `thread` | `RyzenLinux` (container) | Clang 23, TSan, libc++, `vm.mmap_rnd_bits=28` on the host | 4.4.4 |
| `web-integration-gcc` | `web-integration` / `gcc` | `RyzenLinux` (container) | GCC of Ubuntu 24.04 (effective version reported, not GCC 16), CEF | 4.4.4 |
| `web-integration-clang` | `web-integration` / `clang` | `RyzenLinux` (container) | Clang 23, CEF | 4.4.4 |
| `mddlog-clang-source-install` | `clang` | `RyzenLinux` (container) | Clang 21, libc++, source then installed mddlog | 4.3.1 |
| `mddlog-sanitizers-address-undefined` | `sanitizers` / `address-undefined` | `RyzenLinux` (container) | Clang 21, libc++, ASan+UBSan, Catch2 run directly | 4.3.1 |
| `mddlog-sanitizers-thread` | `sanitizers` / `thread` | `RyzenLinux` (container) | Clang 21, libc++, TSan, Catch2 run directly | 4.3.1 |

mddlog lanes use the revision pinned in `MddlogIntegration.yml`; CMake 4.3.1 stays in the qualified 4.0 to 4.3 window.

## Drift check

`cmake/local-ci/lanes.json` is the single source. The CTest case `LocalCiLanesMatchWorkflows` runs
`test/cmake/CheckLocalCi.cmake` offline on the workflows as text and fails when:

- a workflow job or matrix value has no lane, or a lane matches no job or matrix value; duplicate values, additional matrix axes,
  exclusions and unsupported matrix syntax are rejected explicitly;
- the build type, an option, a target, a CTest label or argument, a sanitizer flag, an environment variable, a sysctl, the fuzzed
  decoders or their duration, the compiler or the pinned CMake version of a lane differs from its job;
- the mddlog revision or repository of a lane differs from the workflow;
- this document does not name a lane;
- the declaration of the local differences below is removed.

Configure options, build targets and CTest arguments are compared in both directions, after substituting the selected matrix row
and the sanitizer's string conditional. An additional option or test filter is drift too. Each switch keeps its operand during
comparison. Every configuration must have a build and, for CTest lanes, a test invocation on the same build directory. The mddlog
source and installed modes are checked separately; direct Catch2 arguments and repeat counts are checked as well.

The reader supports the workflows' current block mappings, a single inline matrix axis with optional `include` rows, or an
`include`-only matrix. It reads scalar, folded and literal `run` scripts and direct `cmake`, `ctest` and Catch2 command lines.
It is not a general YAML or shell interpreter: a new matrix shape or unresolved matrix expression in a checked command must be
supported explicitly before the check can pass. Other evidence (compiler setup, environment and provisioning steps) remains a
textual check.

`LocalCiLanesRejectDrift` mutates copies of the workflows and of the lanes, checking additions, removals, swapped sanitizer rows,
new axes and lost test invocations. It also checks that harmless argument and matrix-row reordering passes.
Workflows with CRLF line endings exercise both the mutation reader and the checker; CMake's `file(READ)` already normalizes them.

The command metadata in `lanes.json` uses these fields:

- `options` and `common_options` are the configuration's CMake definitions; `build_type` supplies `CMAKE_BUILD_TYPE`.
- `workflow_options` records the GitHub-specific cache definitions, checked exactly despite the declared local cache difference.
- `toolchain_file` supplies `CMAKE_TOOLCHAIN_FILE`. `configure_variants` adds each configure invocation's definitions in workflow
  order (one variant by default), including source versus installed mddlog paths and sanitizer flags.
- `ctest.workflow_args` records GitHub's JUnit output and optional desktop repetition, in addition to `ctest.args` and the labels.
- `@workspace@` normalizes `github.workspace` paths. Other GitHub expressions are compared literally with whitespace removed;
  the checker does not evaluate event-dependent desktop repetition.

## Known differences with GitHub

- Parallelism: GitHub builds with `--parallel 2`; local lanes use the jobs limit of their machine (4 simultaneous compilations on
  Windows).
- Cache: GitHub restores `actions/cache` and sccache; local lanes keep persistent CPM and ccache volumes. The gain is to be measured,
  not assumed. The desktop's `ENABLE_CACHE=OFF` and explicit sccache launcher are recorded in `workflow_options`.
- Reports and repetition: GitHub's JUnit paths and the desktop's event-dependent `--repeat` are recorded in `ctest.workflow_args`;
  local result collection and stability-run selection belong to the orchestrator delivered in the later steps.
- Toolchains: GitHub installs them per run; local machines are prepared once.
- Runner images: `ubuntu-26.04`, `ubuntu-24.04`, `windows-2025` and `macos-26` are replaced by pinned containers and by the host
  systems. The effective compiler and, on macOS, the selected Xcode are reported and compared with the `latest` of the runner.
- Steps with no local equivalent: artifact uploads (the orchestrator collects the logs), `sccache-action`, `actions/cache`, and the
  isolation of the public network, which applies to the lane container instead of the host firewall. The workflows contain no Codecov,
  Gist or badge step; coverage stays local as `coverage.info` and `lcov --list`.

## SSH/Linux execution (delivery step 2)

The Python 3.10+ driver uses the standard library only. The invoking workstation needs Git, OpenSSH (`ssh`) and Python;
Docker and the toolchains are needed only when preparing the images/dependencies and on the Linux execution host. There is
no `uv`, JavaScript runtime or package manager dependency. Run from the checkout to test:

```sh
python3 cmake/local-ci/run pack --dependencies /path/to/prepared-kit --output /path/to/kit.tgz
# pack prints the SHA-256 of kit.tgz; use that exact value below.
python3 cmake/local-ci/run run --commit HEAD --machines RyzenLinux \
  --kit /path/to/kit.tgz --kit-sha256 <sha256> --parallel-lanes 2 --jobs 4 --require-all
```

`--machines` and `--lanes` take comma-separated names; both filters apply when both are present. Without filters, all ten lanes
are selected. `--require-all` requires every **selected** lane to pass; a filtered run still reports `INCOMPLETE` for the full
matrix. `--keep` retains remote worktrees and builds. `--timeout` bounds each lane (default 3600 seconds), and
`--transfer-timeout` bounds preparation (default 1800 seconds). Limits must be positive. SSH probes have a five-second connection
timeout and a ten-second overall deadline. An unknown name or an empty selection is an error.

The driver refuses tracked or untracked changes. `--allow-dirty` explicitly permits them, but tests the named **commit only**,
without sending the changes. Lane metadata comes from that commit, not from dirty files. The driver sends a self-contained Git
bundle using a temporary private reference, so detached or unpushed commits work. It removes the reference afterwards; neither
side pushes or fetches a remote repository. The remote clone mirrors the private reference before creating independent detached checkouts. Their standalone `.git`
metadata remains usable at `/workspace` inside a container, without absolute paths into the host clone.
The orchestration code itself comes from the driver's installation.

Each run has a unique ID. Logs, `results.json` and per-lane artifact tar files land under
`build-local-ci/<full-sha>/<run-id>/`, which the existing `build*/` ignore covers. Artifacts include CTest JUnit files,
`LastTest.log`, CMake configure diagnostics, fuzz findings and `coverage.info`, when produced. The coverage summary is also in the
lane's log. A failed test, timeout, interruption or artifact-collection failure returns nonzero. No executed lane also returns
nonzero. Without `--require-all`, unreachable/skipped lanes permit a successful partial result only when another lane passes;
the printed matrix is still explicitly incomplete. Ctrl+C requests remote container removal, joins the worker threads and writes
results; inability to confirm remote cancellation is recorded in the lane log. An SSH outage can prevent confirming cancellation:
the remote worker also enforces its own deadline and handles SIGHUP/SIGTERM.

### Preparing the Linux account

Create a dedicated unprivileged account, with key-only SSH authentication and no access to the primary account's data or
credentials. Use **rootless Docker** for this account; membership in a rootful Docker group grants host administrator access
and does not satisfy this account boundary. Install Git, Python 3.10+, `cp`, `sysctl` and rootless Docker. Do not copy GitHub,
Codecov or Gist tokens, SSH agent sockets or Docker credentials. The worker rejects UID 0. The account owns only its working
root (`~/.webfront-local-ci/runs/`), its persistent cache (`~/.webfront-local-ci/cache/`) and its rootless Docker image store.
No host firewall, sysctl or system package setting is changed by a run. Unless `--keep` is given, the driver removes its own
unique run directory after collection; images and caches remain for reuse.

Configure the workstation's existing SSH client, accepting/verifying host keys during setup:

```sshconfig
Host RyzenLinux
    HostName <linux-host-address>
    User webfront-ci
    IdentityFile ~/.ssh/webfront-ci
    IdentitiesOnly yes
Host CurmudgeonWindows
    HostName <windows-host-address>
    User webfront-ci
    IdentityFile ~/.ssh/webfront-ci
    IdentitiesOnly yes
Host Mac
    HostName <mac-host-address>
    User webfront-ci
    IdentityFile ~/.ssh/webfront-ci
    IdentitiesOnly yes
```

For both TSan lanes the **owner** must provision `vm.mmap_rnd_bits=28` on RyzenLinux. The worker checks `sysctl -n` before any
container starts and fails with the actual value and required setting when they differ. This is a host setting, not a Docker
namespace setting. The orchestrator never changes it.

### Preparing the offline kit on the workstation

Provision separately from execution. Use the pinned dependency versions of the **commit under test**, including transitive
ones. The initial version seals and checks a **prepared** dependency directory; it does not automatically download or resolve
it. Verify upstream archive checksums and Git revisions while preparing it. The kit's generated per-file SHA-256 inventory and
the separately supplied archive SHA-256 detect subsequent alteration/transfer errors; they do not authenticate an upstream URL.
`provision.json` records the provenance so it can be audited. No dependency source tree may contain `.git` metadata, links,
credentials or device files. Export Git sources with `git archive <verified-commit>` and dereference internal source links before
packing. The packer rejects links and Git metadata rather than copying local repository credentials.

Use this directory layout (unneeded toolchains may be omitted for a filtered run):

```text
prepared-kit/
  provision.json
  preload.cmake
  cpm/CPM_0.43.2.cmake
  sources/stdnet/           # 8005774eabcba7e73188c02d2eeca2ff43c4ee3a
  sources/stdexec/          # stdnet's STDNET_STDEXEC_TAG, currently 7df6de8eeec667c8adce66fa5a3e17cfd6849e48
  sources/Catch2/           # v3.15.2
  sources/CEF/              # v154.0.34 wrapper package
  cef-binaries/             # unpacked SDK selected by that CEF package, Linux architecture of the host
  bootstrap/environment.tgz
  registry/                 # every archive in test/web/registry-packages.json, using WebFrontRegistry's scoped names
  mddlog/                   # sources at lanes.json's mddlog.ref, without .git
  images/                   # docker save archives, not credentials or registry configuration
```

The bootstrap URL and SHA-512 are in `cmake/WebFrontBootstrap.cmake`; registry URLs and SHA-512 values are in
`test/web/registry-packages.json`. CMake verifies both again during each lane. CPM's own version is pinned in `cmake/CPM.cmake`;
its module is seeded into the persistent CPM cache during preparation. stdnet/Catch2 and CEF references are in the root,
`test/unit/` and `cmake/CEFIntegration.cmake` respectively. Include any additional sources required by mddlog's pinned revision
and its toolchain, overriding their fetches in the preload. Missing transitive dependencies fail offline. No warm workstation
cache is implicitly available to the remote container.

A minimal `preload.cmake` for the WebFront dependencies is:

```cmake
set(CPM_stdnet_SOURCE /deps/sources/stdnet CACHE PATH "")
set(CPM_Catch2_SOURCE /deps/sources/Catch2 CACHE PATH "")
set(CPM_CEF_SOURCE /deps/sources/CEF CACHE PATH "")
set(FETCHCONTENT_SOURCE_DIR_STDEXEC /deps/sources/stdexec CACHE PATH "")
```

Do not set `WEBFRONT_MDDLOG_SOURCE_DIR` globally in the preload: the installed-mddlog variant must use its installed package.
The worker supplies the source path only for the source variant. It also supplies the bootstrap/registry directories, ccache,
CPM cache and `FETCHCONTENT_FULLY_DISCONNECTED=ON`. For CEF it copies `cef-binaries` to the lane's private
`build-local-0/_deps/cef_binaries-src` and sets the matching FetchContent override. This preserves the paths used by
`CEFSupport.cmake` and lets its resource-copy rules write into a private SDK instead of a shared or read-only source tree.

`provision.json` maps each runtime key in `lanes.json` to an immutable Docker image **ID** (the `sha256:...` returned by
`docker image inspect`, not a mutable tag). Archive paths are relative to the kit. For example:

```json
{
  "images": {
    "linux-ubuntu24-clang23": {"id": "sha256:<64 hexadecimal digits>", "archive": "images/noble.tar"},
    "linux-ubuntu26": {"id": "sha256:<64 hexadecimal digits>", "archive": "images/desktop.tar"},
    "linux-ubuntu-clang21-libcxx": {"id": "sha256:<64 hexadecimal digits>", "archive": "images/mddlog.tar"}
  },
  "mddlog_ref": "5b43265664d83c3b7135d328f611e685792a1b02",
  "sources": {
    "preload.cmake": {"origin": "local reviewed fetch overrides", "integrity": "<reviewed SHA-256>"},
    "cpm": {"origin": "CPM.cmake release URL", "integrity": "<verified archive SHA-256>"},
    "sources": {"origin": "<repository URLs and exact commit IDs for every exported tree>", "integrity": "<verified revisions/archive hashes>"},
    "cef-binaries": {"origin": "<CEF SDK archive URL>", "integrity": "<verified upstream archive hash>"},
    "bootstrap": {"origin": "<pinned bootstrap URL>", "integrity": "<pinned SHA-512>"},
    "registry": {"origin": "test/web/registry-packages.json at <commit>", "integrity": "<pinned SHA-512 values>"},
    "mddlog": {"origin": "ambroise-leclerc/mddlog at <pinned ref>", "integrity": "<verified commit>"},
    "images": {"origin": "linux.Dockerfile and pinned base digests, build arguments and CMake release URLs", "integrity": "<image IDs and verified CMake archive hashes>"}
  }
}
```

Every top-level entry requires provenance. `pack` adds `kit.json`, containing a SHA-256 for every file, to the archive without
modifying the input directory. The remote worker checks the outer archive hash before extracting, rejects paths escaping the
run directory and checks every extracted file (including missing and unexpected files) before loading images. Docker loads the
supplied archives, verifies the declared image IDs exist, then uses `--pull=never`.

### Linux images and execution

`cmake/local-ci/linux.Dockerfile` defaults to a digest-pinned Ubuntu 24.04 base, GCC 13, LLVM 23 and CMake 4.4.4. Build on the
provisioning workstation. Its context must contain the unpacked, checksum-verified CMake release under `cmake/`. Build a second
image with `LLVM_VERSION=21 CMAKE_VERSION=4.3.1` and the matching CMake archive for mddlog, and a desktop image with
`BASE_IMAGE=ubuntu@sha256:<verified-26.04-digest> GCC_VERSION=16` and CMake 4.4.4. Pin any overridden base **by digest**, never a tag.
Both the default `gcc`/`g++` metapackages and the requested versioned compiler packages are installed and probed during image
construction. The LLVM archive key fingerprint is checked before enabling the apt repository. Package repositories are used only at image
build time; the saved image ID seals the resulting toolchain. Record each image's base, arguments and CMake archive checksum in
`provision.json`. Save the images on the workstation with `docker save --output <kit>/images/<name>.tar <image>`.

Each Linux lane uses a disposable container with the invoking account's UID/GID, dropped capabilities, `no-new-privileges`,
Docker's default seccomp profile and a private 1 GiB `/dev/shm`. The worker checks that Docker reports rootless mode.
A supplementary container group 0 maps to the dedicated host account's group; setgid build/cache directories and umask 002
let the non-root container write shared build and cache files. Private browser profiles may still be mode 0700;
a final offline container reuses the test UID/GID to remove workspace contents before the host removes the empty directory.
This cleanup mounts only that workspace, drops all capabilities, and is omitted with `--keep`. Git's safe-directory exception is limited to `/workspace`. No privileged mode, Docker socket, host network or host IPC is
mounted. Existing CEF test policy disables Chromium's internal sandbox; isolation is provided by the unprivileged, disposable
container. There is no additional `seccomp=unconfined` exception. WebKit's workflow sandbox exception applies only to its trusted
CTest fixtures. Xvfb is installed in the image; the existing CTest commands start it where needed.

All phases, including **configure and build**, use `--network=none`. SSH runs on the host and remains reachable; only container
loopback is available to browser fixtures. This also prevents a CMake fallback downloader from contacting GitHub. Source and
installed mddlog modes, six direct Catch2 runs for each mddlog sanitizer, all four fuzzers, native WebKitGTK and coverage use the
commands generated from the lane metadata. The container reports and checks the effective CMake/compiler major versions;
the CEF GCC lane reports Ubuntu's default compiler rather than claiming GCC 16. Builds run concurrently up to `--parallel-lanes`,
with `--jobs` compiler jobs each. Persistent CPM/ccache directories are mounted under `/cache`; the kit source overrides remain
in the per-run read-only dependency tree. There are no cache-speed or memory-use claims until measured on RyzenLinux.

### Qualification still required

`test/cmake/TestLocalCi.py` runs offline with Python 3.10+ and is registered as `LocalCiOrchestrator` when that interpreter is
available (Python remains optional for normal C++ consumers). It tests the command plans, tar safety/integrity, unpushed bundle
transport, failing tests, unreachable machines, dirty trees, timeouts, interruption, TSan preconditions and container isolation
arguments. Its protocol test substitutes local SSH/Docker executables; it does **not** qualify actual toolchain images or GUI
sessions. `LocalCiLanesMatchWorkflows` and its negative suite still check the workflows independently.

Actual Linux image builds, the eight lanes on RyzenLinux, warm-cache timings and comparison with GitHub on the same commit
remain to be qualified. Delivery step 3 still needs the Windows interactive scheduled task (MSVC environment, at most four
compilations and WebView2 runtime), the macOS graphical session and Xcode reporting, and the ten-lane run. For now a reachable
Windows/Mac machine reports `skipped (native session support pending step 3)`; an unreachable one reports `unreachable`.
Neither counts as passed, and `--require-all` fails when either is selected. The story remains open.
