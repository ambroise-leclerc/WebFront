# Local CI matrix

Story #346: run the whole CI matrix of WebFront from a development machine, over SSH, on three machines of the local network
(`RyzenLinux`, `CurmudgeonWindows`, `Mac`), to validate a commit before pushing it. The machines stay under the sole control of
their owner: no self-hosted GitHub runner, no inbound connection from GitHub, no GitHub secret on the machines. GitHub CI remains the
reference for pull requests.

## State of the delivery

The story is delivered in three verifiable steps; a partial step does not close it.

| Step | Content | State |
| --- | --- | --- |
| 1 | The eleven lanes in `cmake/local-ci/lanes.json` and the offline drift check `LocalCiLanesMatchWorkflows` | delivered |
| 2 | Commit transport, offline-kit verification, Linux containers and the orchestrator | implemented; real-host/image qualification pending |
| 3 | Native Windows and macOS sessions, result collection, full qualification and timings | Windows and macOS implementations delivered; real-host qualification and timings pending |

## The lanes

`BuildAndTest.yml` expands to eight lanes and `MddlogIntegration.yml` to three. The workflows' trigger conditions (push, schedule,
manual dispatch) do not exempt the local matrix from the extended lanes: a full run selects all eleven.

| Lane | GitHub job | Machine | Toolchain | CMake |
| --- | --- | --- | --- | --- |
| `performance-linux-gcc-webkitgtk` | `performance-linux` | `RyzenLinux` (container) | GCC 16, Release, WebKitGTK, three performance runs | 4.4.4 |
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

`--machines` and `--lanes` take comma-separated names; both filters apply when both are present. Without filters, all lanes in the selected commit
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
Before any SSH probe or result-file creation, all lane machines must be one of `RyzenLinux`, `CurmudgeonWindows` or `Mac`,
and lane IDs must be unique lowercase alphanumeric/hyphen names of at most 64 characters, beginning with a letter or digit.
Workers validate the same metadata and reject unknown requested lanes before using them in paths. SSH also ends its option
list explicitly before the machine name.

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

For both TSan lanes the **owner** must provision `vm.mmap_rnd_bits=28` on RyzenLinux. The worker checks it before any
container starts and fails with the actual value and required setting when they differ. This is a host setting, not a Docker
namespace setting. The orchestrator never changes it. The kernel keeps the file root-only (mode 0600), so the dedicated
unprivileged account cannot run `sysctl -n` on it. The worker then measures the effective entropy of `mmap` over 96 fresh
processes and treats the span of their base addresses, minus the one bit that the base address subtraction's borrow adds, as the
value. The estimate is not an attestation of the sysctl; the owner remains responsible for provisioning it.

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
CPM cache and `FETCHCONTENT_FULLY_DISCONNECTED=ON`. It also exports `CPM_SOURCE_CACHE=/cache/cpm` so independent CMake
configurations launched by CTest, including `AddSubdirectoryConsumer`, reuse the prepared CPM module without downloading it.
For CEF it copies `cef-binaries` to the lane's private
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
Windows extraction additionally rejects reserved device names (such as `aux.h` and `nul.c`), alternate data streams,
trailing dots/spaces and case-insensitive path collisions. Linux workers retain case-sensitive POSIX names, so `Foo`/`foo`
and ordinary files named `aux.h` remain valid in Linux kits. Absolute paths, drive paths, parent traversal, backslash paths,
links and exact duplicate archive entries remain forbidden for every target. Extraction onto a Windows filesystem always
applies the stricter Windows rules, regardless of the kit's target.

### Linux images and execution

`cmake/local-ci/linux.Dockerfile` defaults to a digest-pinned Ubuntu 24.04 base, GCC 13, LLVM 23 and CMake 4.4.4. Build on the
provisioning workstation. Its context must contain the unpacked, checksum-verified CMake release under `cmake/`. Build a second
image with `LLVM_VERSION=21 CMAKE_VERSION=4.3.1` and the matching CMake archive for mddlog, and a desktop image with
`BASE_IMAGE=ubuntu@sha256:<verified-26.04-digest> GCC_VERSION=16` and CMake 4.4.4. Pin any overridden base **by digest**, never a tag.
Both the default `gcc`/`g++` metapackages and the requested versioned compiler packages are installed and probed during image
construction. The versioned `llvm-N` package supplies `llvm-ar` and `llvm-ranlib`, which mddlog's `linux-clang21-libcxx` toolchain requires. The LLVM archive key fingerprint is checked before enabling the apt repository. Package repositories are used only at image
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

Actual Linux image builds, the nine lanes on RyzenLinux, warm-cache timings and comparison with GitHub on the same commit
remain to be qualified. The Windows implementation below still needs qualification on CurmudgeonWindows with its dedicated
account and network policy. The macOS implementation below also needs qualification with its dedicated Aqua account and full Xcode installation,
and all eleven lanes must still run together. Missing macOS graphical sessions report `skipped (no interactive session)`;
unreachable machines report `unreachable`. Neither counts as passed, and `--require-all` fails when either is selected.
Although GitHub currently marks #346 closed, the full acceptance evidence remains incomplete.

### Native Windows execution

The Windows lane uses the same commit bundle, verified kit inventory, command metadata and result collection as Linux.
It runs natively, with Ninja and the latest installed x64 MSVC toolset selected by `vswhere`, in the dedicated account's
graphical session. It prints the effective compiler banner, CMake and WebView2 versions. CMake must match the lane's pinned
version exactly; `latest` MSVC is reported, without claiming equality with the current GitHub runner image.
MSVC deliberately follows the latest installed x64 toolset and WebView2 follows the installed Evergreen runtime, matching the
workflow's unpinned toolset/runtime policy. Unlike CMake, their versions are recorded rather than compared with an expected
version: divergence from GitHub requires reviewing those logs. Runtime discovery establishes that a versioned executable
exists; the browser tests establish actual compatibility. No automatic toolset or Evergreen-version parity is claimed.
Compilation is capped at four jobs, even when `--jobs` requests more. CTest retains the metadata's full test selection,
Release configuration, parallelism and `--no-tests=error`, and writes JUnit output.

Prepare a dedicated **standard**, non-administrator Windows account with Git, Python 3.10+ (`python.exe` on its SSH PATH,
not the Store alias), Ninja, sccache, the pinned CMake, Visual Studio C++ x64 tools/Windows SDK and `vswhere` installed.
Install WebView2 Evergreen separately while provisioning. Execution never installs or updates the runtime.
Enable key-only OpenSSH access and verify its host key. Log that same account on locally (or in an active RDP session),
and keep its desktop unlocked during tests. A service/SSH session or a disconnected graphical session is insufficient.

For each run the SSH worker registers a uniquely named `WebFrontCI-<run>-<lane>` task for the current account with
`LogonType Interactive`, `RunLevel Limited`, no password, no elevation and an execution deadline. It starts that task on demand.
The task checks its active session/input desktop before configuring; failure is a failed lane, never a pass. If no session
worker starts within 15 seconds, execution fails with an account/session diagnostic. The task obtains its MSVC environment
from `VsDevCmd.bat`, uses a private checkout and writes a session log plus an atomic result file. A Windows Job Object with
`KILL_ON_JOB_CLOSE` owns the task process and descendants. A timeout, Ctrl+C or failed SSH run requests task stop through a
second SSH channel; the native worker also enforces its deadline and the scheduled task has a backup time limit.
Task stop is confirmed before unregistering and deleting the run. Cancellation failure preserves files for inspection.
Normal completion unregisters the task, collects the session log, JUnit, `LastTest.log` and CMake diagnostics, then removes
the checkout (including read-only Git objects). `--keep` preserves files after stopping the task.
If stop fails, the worker still attempts an artifact snapshot and prints the session log, retains the checkout, and reports
the stop failure without replacing the original execution error. A collection failure also retains the source diagnostics.
Snapshots taken while stop is unconfirmed may be incomplete because the task could still be writing files.

Task start/finalization, recovery stop, artifact streaming and run cleanup use a shared per-run cross-process lock. Recovery
stop collects diagnostics too. A late execution channel cannot overwrite an already collected archive after checkout removal,
or recreate a run removed by cleanup. Lock files live outside the removable run under `.webfront-local-ci/locks/`; the small
files are deliberately retained so late channels always lock the same file. Recovery SSH commands allow 90 seconds for
serialization and cancellation. Actual OpenSSH-for-Windows process behavior on a lost channel remains a host qualification
item; correctness of collection/cleanup does not depend on receiving SIGTERM at disconnect.

Windows needs its own kit: use `"platform": "windows"`, `"images": {}` in `provision.json`, the same provenance mapping,
CPM module, exported stdnet/stdexec/Catch2, bootstrap/registry archives, and the pinned WebView2 SDK exported as
`sources/WebView2SDK`. Its URL, version and SHA-256 are in `cmake/WebView2Support.cmake`. Neither Docker nor Linux SDKs
are needed. A portable preload uses paths relative to itself:

```cmake
set(CPM_stdnet_SOURCE "${CMAKE_CURRENT_LIST_DIR}/sources/stdnet" CACHE PATH "")
set(CPM_Catch2_SOURCE "${CMAKE_CURRENT_LIST_DIR}/sources/Catch2" CACHE PATH "")
set(CPM_WebView2SDK_SOURCE "${CMAKE_CURRENT_LIST_DIR}/sources/WebView2SDK" CACHE PATH "")
set(FETCHCONTENT_SOURCE_DIR_STDEXEC "${CMAKE_CURRENT_LIST_DIR}/sources/stdexec" CACHE PATH "")
```

CPM's module and the sccache data persist under the dedicated account's `.webfront-local-ci/cache/`.
The worker exports `CPM_SOURCE_CACHE` to the graphical session, so independent configurations launched by CTest reuse the
verified module as well; supplying only the top-level `-DCPM_SOURCE_CACHE` would let those configurations attempt a download.
Each lane uses a separate sccache server port; stopping its Job does not stop a concurrent run's server.
The verified dependency sources remain private to the run. FetchContent is disconnected; missing CPM/transitive dependencies
also fail under the public egress policy rather than downloading. The packer seals prepared files; it does not resolve them.

Windows network policy must be prepared by the owner on a **dedicated CI host or VM**. The worker never changes firewall
rules. It requires all Windows Firewall profiles enabled and the effective outbound block rule `WebFrontLocalCI-BlockPublic`
covering every program, protocol and interface and the complete public IPv4/IPv6 address space. To review the setup commands:

```powershell
python.exe cmake/local-ci/windows_worker.py --print-firewall-setup
```

Apply those commands once as administrator on the dedicated host, after reviewing them. They affect all host users/programs.
The block deliberately exempts RFC1918 IPv4, loopback, link-local and IPv6 unique-local addresses, preserving private-LAN SSH
and browser loopback. Consequently **LAN access remains possible**; this is weaker isolation than Linux `--network=none`.
The complement also deliberately blocks IPv4 multicast (`224.0.0.0/4`), limited broadcast (`255.255.255.255`) and IPv6
multicast (`ff00::/8`). This may affect DHCP renewal, mDNS, LLMNR and IPv6 neighbor discovery. Qualify those services and SSH
reachability across lease renewal on the dedicated host before enabling the policy; no multicast/broadcast exception has
been validated or added. Existing allowance of `fc00::/7` does not imply multicast traffic is allowed.
Do not expose a public-network proxy on the permitted LAN or loopback. The worker checks the active policy during preparation
and again in the interactive task, before configure/build/tests. Rule absence, disabled profiles or narrower coverage fails.
The policy must remain enforced by the owner throughout the run; this native account is not a sandbox for hostile commits.

Seal the kit on a prepared workstation and run the Windows lane from any supported orchestrator host:

```powershell
python.exe cmake/local-ci/run pack --dependencies C:/kits/windows-prepared --output C:/kits/windows.tgz
python.exe cmake/local-ci/run run --machines CurmudgeonWindows --commit HEAD `
  --windows-kit C:/kits/windows.tgz --windows-kit-sha256 <sha256> --jobs 4 --require-all
```

`--windows-kit` and `--windows-kit-sha256` must be supplied together. They are independent of Linux's `--kit` and
`--kit-sha256`, so one invocation can select both machines and send the appropriate kit to each. The SSH transport uses
encoded PowerShell commands and preserves spaces, apostrophes and binary streams with either cmd.exe or PowerShell as the
OpenSSH default shell. No account, scheduled task or firewall needs to be changed on the orchestrator host.

`LocalCiWindows` adds offline regressions for native command generation, task policy, kit integrity, network preconditions,
failed/successful sessions, missing desktops, deadlines, cancellation, artifact retention and cleanup. Its full protocol test
uses an SSH/native-session stand-in with real bundles/checkouts and artifacts. On Windows it also exercises real PowerShell
binary transport and a real Job Object's child-process termination. These tests do not register real tasks or change firewall
policy. Running the whole lane over SSH with the dedicated graphical account, cold/warm caches and comparison against GitHub
remain qualification work; implementation tests are not evidence that the remote host has been provisioned correctly.
In particular, verify from the actual **non-elevated SSH account** that `Get-NetFirewallProfile` and
`Get-NetFirewallRule -PolicyStore ActiveStore`, plus every `Get-NetFirewall*Filter` used by `checkNetwork`, can read the
effective policy. These permissions have not been tested on the dedicated host: denial fails preparation before any build,
and the worker does not elevate or bypass the check. Also qualify public egress blocking, multicast/broadcast effects,
MSVC/Evergreen version differences, and a real SSH disconnect followed by recovery collection and cleanup.


### Native macOS execution

The Mac lane now transfers an unpushed commit and a separate `platform=macos` kit over SSH, then runs the AppleClang/Release
configuration with `WEBFRONT_EMBED_WKWEBVIEW=ON` and `WEBFRONT_WEBTEST_FRONTEND=WKWebView`. All metadata-selected CTest cases,
including browser tests, run with `--no-tests=error`, the declared parallelism and JUnit output. Compilation is capped at four
jobs. Local macOS uses ccache (without a background server) instead of GitHub's sccache; the verified CPM module and compiler
cache persist under the dedicated account's `.webfront-local-ci/cache/`. CTest child configurations inherit `CPM_SOURCE_CACHE`.

Prepare a dedicated **standard, non-administrator** macOS account without the principal user's files, credentials, keychain
items or cloud sessions. Enable key-only Remote Login for that account, verify the host key, and configure the development
workstation's `Mac` SSH alias to select it explicitly. Install Python 3.10+ (`python3` on the SSH PATH), Git, Ninja, ccache,
the lane's exact CMake version and full Xcode before execution. Accept Xcode's license and complete its first-launch setup
while provisioning; select it with `xcode-select` then. Command Line Tools alone are insufficient: `xcodebuild -version`
must succeed. No installer, `sudo`, Xcode selection change or runtime update runs during a lane.

Log the **same dedicated account** into an unlocked, active graphical console session. SSH alone and a background `user/UID`
domain do not provide Aqua. A unique, one-shot LaunchAgent is bootstrapped into `gui/UID`, with its plist/profile/logs inside
the disposable run directory (no installation in `~/Library/LaunchAgents`). It inherits an explicit tool PATH, HOME and locale,
without forwarding the SSH environment, tokens or proxy variables. It checks console ownership, the Aqua domain and the
CoreGraphics session's user, login and on-console state, plus IORegistry's lock state for that audit session (an omitted lock key means unlocked) before building. No graphical session or a locked/inactive
session returns worker code 125 and `skipped (no interactive session)` in the summary; `--require-all` fails. Failure to start
an agent despite a usable domain is a failed lane with a launch log, never a pass or a silent skip.

The log records `xcode-select -p`, `xcodebuild -version`, `xcrun --find clang++`, the effective AppleClang banner and macOS
version. CMake must match `lanes.json` exactly. GitHub's `xcode-version: latest` is mutable: compare these records against the
same-commit GitHub run, recording any difference. There is no automatic claim that the host matches GitHub's Xcode or OS image.

The agent starts under `/usr/bin/sandbox-exec` with an inherited network policy: ordinary worker/build/test descendants can
use IPv4/IPv6 loopback and Unix sockets, but cannot connect directly to external IP addresses. A probe checks that IPv4 and
IPv6 connects are denied before any CMake command executes. Missing sandbox support or an ineffective restriction fails closed.
This keeps a CMake fallback download from reaching GitHub. The worker does not alter the host firewall. **This process policy
does not constrain system XPC services started outside the agent's process tree**, including WebKit services. Unix sockets are also unrestricted: a tested process can contact a local service that performs
network access on its behalf, so the policy does not prevent indirect Internet access through such services. It is not a
complete network isolation boundary or a sandbox for hostile commits. The owner must qualify WebKit's actual network behavior
and, where strict host-wide egress isolation is required, provision that isolation separately on the dedicated Mac or network.
Do not expose a public-network proxy on loopback. Full acceptance of #346's no-public-network criterion remains pending this
qualification; direct-socket regression tests alone do not establish it for XPC services. `sandbox-exec` is deprecated and may
be unavailable on future macOS releases; such hosts fail rather than running unrestricted.

The SSH coordinator passes one absolute host-monotonic deadline to the agent, which enforces it alongside a cancellation
marker. After expiry, the coordinator allows five seconds for the agent to flush its log and publish code 124 before stopping
it. A named 15-second startup warning threshold is capped by the lane timeout; it warns without aborting a slow bootstrap
before the shared deadline. Preparation bounds its local bundle clone to 300 seconds. Launchd owns its process group (`AbandonProcessGroup=false`); finishing,
timeout, interruption and SSH recovery boot out the service with `launchctl bootout --wait` under a bounded subprocess timeout.
The host must support that option. Stop failures preserve files and fail the run. A per-run cross-process lock serializes
start/finalization, recovery stop, artifact streaming and cleanup; late channels cannot recreate deleted runs. Logs (including
sandbox/launch failures), JUnit, CTest and CMake diagnostics are archived before checkout removal. `--keep` retains the run after
stopping it. Native process groups do not contain deliberately detached processes or system XPC services; only trusted commits
are supported. Real disconnect and descendant cleanup behavior still needs qualification on the dedicated host.

Prepare the same CPM/stdnet/stdexec/Catch2, bootstrap and registry exports as Windows, omitting WebView2SDK and all Docker/CEF
files. Use `"platform": "macos", "images": {}` with provenance for every top-level entry. The Windows preload example above
is portable after removing its WebView2SDK line. Extraction uses the portable Windows filename subset to reject case collisions
on default APFS volumes. No filesystem case-sensitivity assumption is needed.

```bash
python3 cmake/local-ci/run pack --dependencies /kits/macos-prepared --output /kits/macos.tgz
python3 cmake/local-ci/run run --machines Mac --commit HEAD \
  --macos-kit /kits/macos.tgz --macos-kit-sha256 <sha256> --jobs 4 --require-all
```

`--macos-kit` and `--macos-kit-sha256` must be supplied together. A full invocation supplies all three kit/hash pairs (`--kit`,
`--windows-kit`, `--macos-kit`) and omits `--machines`/`--lanes`. Each host receives its own kit; no host needs GitHub access.

`LocalCiMacOS` covers command generation, LaunchAgent configuration, login/lock checks, version reporting, kit integrity,
cache propagation, failure/timeout/cancellation, skipped sessions, cleanup retention and the full bundle/artifact protocol with
a native-session stand-in. On macOS it also exercises the real inherited socket policy, loopback and a child process. It does
not provision an account or register an actual LaunchAgent. Qualification still requires real SSH/Aqua/WKWebView execution,
XPC network checks, disconnect recovery, cold/warm cache measurements, all eleven lanes across three hosts and comparison
against GitHub on the same commit. The development Mac used for implementation has Command Line Tools rather than selected
full Xcode, so it cannot qualify the advertised Xcode lane.
