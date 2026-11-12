# Test tiers (#388)

The suite has three tiers, selected by CTest labels. A test carries at most one of them; a new test with no label belongs
to the standard tier.

| Tier | Label | Content | When it runs |
| --- | --- | --- | --- |
| Fast | `fast` | Catch2 unit tests and the Python tool tests; no browser, no nested build | Locally on demand, and as the first test step of every Linux/Windows/macOS desktop job |
| Standard | none, or `web-integration` | Everything but the portability tests: unit tests, real-browser integration, `wf` sessions, embedded builds | Every pull request and every push to `develop` or `master` |
| Portability | `portability` | The library consumed from another project: `AddSubdirectoryConsumer` | Weekly, and on every pull request into `master` |

```sh
ctest --test-dir build -L fast --parallel 4                # about one second
ctest --test-dir build -LE portability --parallel 4        # the standard tier
ctest --test-dir build -L portability                      # the portability tier
cmake -DBUILD_DIR=build -DLABEL_EXCLUDE=portability -P test/cmake/RunTestSuite.cmake
```

`RunTestSuite.cmake` runs every test unless `LABEL_EXCLUDE` gives a regular expression of labels to leave out. The CI
standard tier passes `^(portability|fast)$`, because its fast step has just run the fast tier. A local run before a
change is declared complete (see `AGENTS.md`) runs everything.

By default every Catch2 scenario is a CTest case of its own, which `ctest -R` selects by name, and a process of its
own: more than 500 of them. `-DWEBFRONT_TEST_UNIT_SHARDS=4`, which the desktop CI jobs pass, registers four cases
`UnitTests-0` to `UnitTests-3` instead, each a Catch2 shard that runs a quarter of the scenarios in one process and
writes the result of each to `unit-results-<shard>.xml` in the build directory. The shards run the scenarios in their
declaration order, which is what makes four processes started separately split them without overlap. Scenarios then
share a process: `test/unit/ScenarioIsolation.cpp` restores the process-wide state a scenario may change. Run directly,
`tests` draws a random order and prints its seed, which `tests --rng-seed <seed>` replays.

## Following a browser test live

`DeveloperBrowserIntegration` and its siblings `DeveloperBrowserIntegration-scripts`, `-typescript`, `-jasmine`, `-terminal`
and `-workspace` (the parts of `test/web/developer-specs.mjs`), the `Automation*` cases and the `WfDriver*` cases run `wf` commands (`create`, `install`,
`build`, `dev`, `preview`, `embed`, ...) that can take several seconds each. CTest hides the output of a passing test, so
to watch one:

```bash
ctest --test-dir build -R DeveloperBrowserIntegration -V        # CTest prints the output as it arrives
ctest --test-dir build -R DeveloperBrowserIntegration -N -V     # prints the command to run `developer_integration` directly
```

`DeveloperBrowserSession` displays all six parts in one visible native window, with a fresh page, project and origin per
part. `DeveloperBrowserIntegration*` keep their original names and check its independent reports. Run
`developer_integration <fixtures> <output> --session` directly to follow the shared window. Configure
`-DWEBFRONT_TEST_SESSIONS=OFF` to compare the separate-process path.

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
dispatch with `repeat_desktop_tests`, not by a pull request into `develop`.

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
  the fast tier, then rebuilding the tests under GCC 13 ASan/UBSan and repeating the fast tier (#525, S2 support). It has no browser frontend. A compile error or a failing fast test under GCC 13 on aarch64 turns it red;
- the address/undefined and thread sanitizers, the five fuzzers, and the GCC and Clang CEF browser integration.

Manual dispatch with `gcc13_arm64=true` adds the GCC 13 ARM64 job and its sanitizer pass
to the extended checks without enabling the repeated desktop matrix. For example:

```bash
gh workflow run BuildAndTest.yml --ref <branch> -f gcc13_arm64=true
```

The portability matrix does not vary the CMake version: `setup-cmake` pins the SHA-256 of each archive it installs, so
another version needs its checksum added first.

## Required checks for `master`

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
