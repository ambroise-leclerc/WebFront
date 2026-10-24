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
| 2 | Commit transport, provisioning without GitHub access, Linux containers and the orchestrator | not started |
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
