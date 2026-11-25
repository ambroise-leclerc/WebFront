# Test tiers (#388)

The suite has three tiers, selected by CTest labels. A test carries at most one of them; a new test with no label belongs
to the standard tier.

| Tier | Label | Content | When it runs |
| --- | --- | --- | --- |
| Fast | `fast` | C++ unit tests (`include/webfront/testing/Bdd.hpp`) and the Python tool tests; no browser, no nested build | Locally on demand, and as the first test step of every Linux/Windows/macOS desktop job |
| Standard | none, or `web-integration` | Everything but the portability tests: unit tests, real-browser integration, `wf` commands, embedded builds | Every pull request and every push to `develop` or `master` |
| Portability | `portability` | The library consumed from another project: `AddSubdirectoryConsumer`, `BddConsumer` | Weekly, and on every pull request into `master` |

```sh
ctest --test-dir build -L fast --parallel 4                # about one second
ctest --test-dir build -LE portability --parallel 4        # the standard tier
ctest --test-dir build -L portability                      # the portability tier
cmake -DBUILD_DIR=build -DLABEL_EXCLUDE=portability -P test/cmake/RunTestSuite.cmake
```

`RunTestSuite.cmake` runs every test unless `LABEL_EXCLUDE` gives a regular expression of labels to leave out. The CI
standard tier passes `^(portability|fast)$`, because its fast step has just run the fast tier. A local run before a
change is declared complete (see `AGENTS.md`) runs everything.

By default every unit-test scenario is a CTest case of its own, which `ctest -R` selects by name, and a process of its
own: more than 500 of them. `-DWEBFRONT_TEST_UNIT_SHARDS=4`, which the desktop CI jobs pass, registers four cases
`UnitTests-0` to `UnitTests-3` instead, each a shard that runs a quarter of the scenarios in one process and
writes the result of each to `unit-results-<shard>.xml` in the build directory. The shards run the scenarios in their
declaration order, which is what makes four processes started separately split them without overlap. Scenarios then
share a process: `test/unit/ScenarioIsolation.cpp` restores the process-wide state a scenario may change. Run directly,
`tests` draws a random order and prints its seed, which `tests --rng-seed <seed>` replays.

The `startup` label selects the cases `<Frontend>StartupTime-cold` and `-warm` (#611), which belong to the standard tier:
`ctest -L startup` runs them alone, and they print the measured times with `-V`. In a parallel run they are `RUN_SERIAL`:
CTest starts them when no other test runs, so the load of the suite is not counted as start-up time. "Cold" means fresh `HOME` and `XDG_*`
directories (Linux: empty GStreamer registry and font cache) and, on Windows, fresh `LOCALAPPDATA` and `APPDATA`; on macOS the
variables have little effect, so the cold case there only guarantees a new profile directory.

## Waiting for an asynchronous state

A unit test waits for an observable state, never for a fixed pause. `requireEventually(condition)` ends the scenario when
the condition does not hold in time, `checkEventually(condition, 5s)` records the failure and returns the result, and
`requireReady(future)` / `checkReady(future, 5s)` wait for a `std::future` or `std::shared_future` without consuming it
(a deferred future, or one without a shared state, fails at once: waiting never makes the first ready). The condition is any callable, a free
function included, evaluated after pauses that grow from 0.1 to 5 ms; the delay is 30 seconds unless given. A failure
reports the source line of the call, the delay and the time waited, with the sections and `info()` messages of the
scenario.

A thread that cannot assert, or a wait that precedes a cleanup (joining a thread), uses `waitUntil(condition, limit)` or
`waitReady(future, limit)` (false at once for a deferred or empty future), which record nothing and return the result, and the scenario asserts it afterwards.

A slow job (sanitizers, emulation) lengthens every delay of the process with `WEBFRONT_TEST_TIMEOUT_FACTOR=<number>`: digits
with an optional fraction such as `2` or `2.5`, anything else being ignored. Set it when CMake configures: it is then
given to every unit test, whose CTest limit grows with it (60 s + 60 s per unit of the factor, rounded up). Running
`tests` directly reads the variable from its own environment. `test/unit/BddSelfTest.cpp` covers these functions.

## Following a browser test live

`DeveloperBrowserIntegration` and its siblings `DeveloperBrowserIntegration-scripts`, `-typescript`, `-jasmine`, `-terminal`
and `-workspace` (the parts of `test/web/developer-specs.mjs`), the `Automation*` cases and the `WfDriver*` cases run `wf` commands (`create`, `install`,
`build`, `dev`, `preview`, `embed`, ...) that can take several seconds each. CTest hides the output of a passing test, so
to watch one:

```bash
ctest --test-dir build -R DeveloperBrowserIntegration -V        # CTest prints the output as it arrives
ctest --test-dir build -R DeveloperBrowserIntegration -N -V     # prints the command to run `developer_integration` directly
```

Each part is a process with its own project, origin and visible window; CTest runs them side by side
(docs/TEST_PERFORMANCE.md records why the shared window of #556 was retired).

Run `developer_integration` with a fixtures directory and an output directory but no `--part=` for the older mode in
which all parts share one project; `--part=app|scripts|typescript|jasmine|terminal|workspace` plays one in its own visible window.

```bash
```

The process prints a `Developer integration: <time>ms begin|end <step> durationMs=...` line for each phase and each
command. The window of `DeveloperBrowserIntegration` also shows, in place of its terminal, every `wf` command the specs
run with its outcome and duration, as if it had been typed. Only the `wf` subcommand is shown, never an argument, an
option value or a path. A failed command appears as `rejected`, with its exit code when it has one, whether or not the
spec expected the refusal. A wait is labelled `timeout (expected)` only for a time limit the spec announces and fires,
and `cancelled (expected)` only when the spec pressed Ctrl+C; any other time limit or cancellation appears as `timeout`
or `cancelled`. This trace comes from the specs (`test/web/developer-specs.mjs`), not from the published environment.

## A quick local build by default

The defaults of the test options are those of the quickest useful build, and the project has no CMake preset to choose
between: `-DWEBFRONT_ENABLE_TESTING=ON` alone compiles the heavy unit tests four to a file
(`WEBFRONT_TEST_UNITY_BATCH=4`), leaves the MSVC code analysis out (`WEBFRONT_MSVC_ANALYZE=OFF`) and does not register
the reproducibility proofs (`WEBFRONT_TEST_REPRODUCIBILITY=OFF`). The CI passes every one of these options explicitly,
so a default can change without changing what a job checks. While working, `ctest -L fast` then the cases the change
touches (`ctest -R <name>`) are enough; the complete suite runs before the work is declared complete.

## What the desktop jobs compile

A pull request into `develop` builds the library, the tests and the test applications, with the MSVC code analysis (`/analyze`)
off and without the examples (`WEBFRONT_MSVC_ANALYZE=OFF`, `WEBFRONT_BUILD_EXAMPLES=OFF`): about 22 % and 190 s of compiler
time on Windows (docs/BUILD_TIME_STUDY.md). The advanced checks below turn both on again, so a warning that only the analysis
reports, or an example that no longer compiles, is found by the weekly run, by the pull request into `master` or by a manual
dispatch with `repeat_desktop_tests`, not by a pull request into `develop`. One exception, since a C6387 of the PTY reached
the release pull request unseen (#628): a pull request into `develop` that changes Windows code (`components/pty/`, the
WebView2 frontend, a file whose path contains `windows` or `win32`, the warning options or the workflow) runs the analysis too.
The compiler cache is not a cause: sccache replays the diagnostics of a cached object, so a cached build reports what a clean
one does.

The proofs that the self-built environment is reproducible and equal to the checked-in bundles (`SelfBuildRepeat`,
`SelfBuildReproducible`, `SelfBuildRefresh`, `SelfBuildRefreshMatchesSources`) build it twice more, about 45 s on a hosted
runner. A pull request into `develop` runs them only when it changes one of their inputs (`js/`, `examples/life/ui/`,
`test/developer/`, `test/web/registry/` and `test/web/CMakeLists.txt`, `include/webfront/system/`, the CMake scripts that
prepare, compare and assemble the self-build project, `LICENSE` and the workflow itself); a push to `develop` and the advanced
checks always run them. They are off by default: `-DWEBFRONT_TEST_REPRODUCIBILITY=ON` adds them to a local build.

## Configuration checks

The cases that configure a CMake project carry the label `configuration` in addition to their tier: the options, the
frontend selection, the module scan, the CEF and WebView2 support, the parent project that adds WebFront as a
subdirectory, and the repetition of the suite itself (`ctest -N -L configuration` lists them). They test the build system, not
the library, and take about 75 s of worker time on a hosted runner. A pull request into `develop` runs them only when it
changes `cmake/`, `CMakeLists.txt`, the CMake modules of `js/core/`, `test/cmake/`, `test/unit/CMakeLists.txt`, the actions
of `.github/actions/` or the workflow itself; a push to `develop`, the weekly run and the full tier always run them.
`AddSubdirectoryNativeConsumer` is not one of them: it compiles the native frontend from a consuming project, so any change
of the headers can break it. Locally, `ctest -LE configuration` leaves them out.

The coverage capture (`lcov`) runs on a push to `develop` or `master` and in the full tier. A pull request still builds and
runs the instrumented Linux tests, but nothing reads a pull request's capture.

## Advanced checks

These run on the weekly schedule (Monday 04:23 UTC), on every pull request whose base is `master`, and on a manual
dispatch with `repeat_desktop_tests`:

- the complete desktop suite on the three desktop platforms, built with the MSVC code analysis and the examples, repeated
  three times with its fixtures reset on each pass, including the portability tests;
- the `portability` job: the consumer project configured with Ninja (GCC on Linux, MSVC on Windows, Apple clang on macOS),
  with Unix Makefiles (Clang 23) and, reported but never blocking until it has run on a hosted runner, the Visual Studio
  2026 generator. There is no compiler cache: a new user compiles everything;
- the `gcc13-arm64` job (#548): GCC 13 on a hosted Linux aarch64 runner (`ubuntu-24.04-arm`), compiling the library, the examples and
  the tests with warnings that are not errors (libstdc++ 13 reports false `-Wnull-dereference` positives, #526), then running
  the fast tier, then rebuilding the tests under GCC 13 ASan/UBSan and repeating the fast tier (#525, S2 support).
  The instrumented configuration uses `WEBFRONT_TEST_TIMEOUT_FACTOR=2` for the CPU rendering tests on ARM64.
  It has no browser frontend. A compile error or a failing fast test under GCC 13 on aarch64 turns it red;
- the `cef-arm64` job (#524): the `web-integration` suite under CEF `linuxarm64` (GCC 13, Xvfb) on `ubuntu-24.04-arm`, weekly and on demand with `cef_arm64=true`, not on pull requests; it fails if the CEF library is not aarch64;
- the `webkitgtk-arm64` job (#524): the same suite through the WebKitGTK 6.0 frontend of Ubuntu 24.04 (GCC 13, Xvfb) on `ubuntu-24.04-arm`, weekly and on demand with `webkitgtk_arm64=true`, not on pull requests;
- the address/undefined and thread sanitizers, the five fuzzers, and the GCC and Clang CEF browser integration.

Manual dispatch with `gcc13_arm64=true` adds the GCC 13 ARM64 job and its sanitizer pass
to the extended checks without enabling the repeated desktop matrix. For example:

```bash
gh workflow run BuildAndTest.yml --ref <branch> -f gcc13_arm64=true
```

`cef_arm64=true` and `webkitgtk_arm64=true` likewise run only the CEF `linuxarm64` or the WebKitGTK aarch64 browser integration. The two aarch64 browser jobs and `gcc13-arm64` share the `cpm-extended-Linux-ARM64-*` key, so whichever finishes first saves it; the bootstrap and registry archives have a key per job. None of them uses a compiler cache (`ENABLE_CACHE=OFF`).

Measured on hosted `ubuntu-24.04-arm` runners with cold caches (run 43745895208): `gcc13-arm64` 25 min (fast tier 9 s, 551 cases, then ASan/UBSan 43 s), `cef-arm64` 9 min (101 cases, 169 s of tests) and `webkitgtk-arm64` 7 min (113 cases, 221 s of tests). A second run (43748323731) restored the CPM, bootstrap and registry caches on both browser jobs (8 and 7 min): the time is the compilation, not the downloads.

The portability matrix does not vary the CMake version: `setup-cmake` pins the SHA-256 of each archive it installs, so
another version needs its checksum added first.

## Required checks for `master`

The on-board [Jetson Orin runner](JETSON_RUNNER.md) is S2 hardware evidence, separate from
the hosted ARM64 jobs. It runs at Monday 01:17 UTC, on pushes to `master`, and on a
maintainer's manual branch dispatch, never on a PR. It builds CEF-off and runs the complete
CTest suite, the CUDA image regression (a skip fails), then WebKitGTK `web-integration`
under Xvfb on Tegra. Persistent ccache/CPM caches, two workers and low CPU/I/O priority
keep its footprint bounded. It is never required for a PR or merge: an offline workstation
does not block either. Three green weekly runs and workstation usability remain the
hardware acceptance criteria, independently of a successful manual validation.

A pull request into `master` must pass all of: the desktop jobs, `portability` (except the experimental Visual Studio
leg), `gcc13-arm64`, `sanitizers` and `web-integration`. GitHub only enforces this once the checks are marked required in the branch
protection rules of `master`; the workflow cannot set that itself.

The fast tier runs once on a pull request: the step that follows it runs everything else, and a failed fast step
fails the job before the browser suite starts. The three passes of the complete tier run the fast tier again each time.

The shared frontend lifecycle runner has a 45-second watchdog which reports the last observed stage and exits with a failure
status when the interface thread cannot be unwound. Forced process exit does not itself terminate browser subprocesses; they
can remain orphaned until the surrounding CI runner or operating system cleans them up. Do not kill the process group from
the watchdog: a shared Xvfb display and other tests may belong to that group. The 45-second deadline can preempt the
more specific errors of scenarios that make multiple successive 30-second waits. Forced exit also loses the process's
unflushed gcov coverage data. Page stage reports are best effort and can be lost during navigation or disconnection.

The `FrontendLifecycleWatchdog` probe bypasses frontend initialization to test the deadline without a display. The ordinary
`CEFLifecycle-*` CTest cases exercise CEF subprocess dispatch and frontend initialization before the watchdog is started.
