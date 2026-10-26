# Test timings and parallelism (#298)

The CI and contributor commands use `ctest --parallel 4`. This bounds concurrent
test processes independently of `cmake --build --parallel 2`. Desktop, sanitizer
and extended CEF jobs retain JUnit durations and `LastTest.log` even on success,
so subsequent runs can be compared without losing the individual test timings.
Manual dispatch with `repeat_desktop_tests=true` opts into the desktop matrix and
uses `--repeat until-fail:3`; normal PR runs execute each case once, and ordinary
manual dispatch still runs only the extended checks.

## Starting point

The [21 October 2026 run](https://github.com/ambroise-leclerc/WebFront/actions/runs/39640036175)
reported these elapsed CTest durations, rather than timeout limits. These are historical test names:
`DeveloperEmbeddedBuild` and `DeveloperEmbeddedIntegration` were removed with the PI starter;
`LifeEmbeddedBuild` and `LifeEmbeddedIntegration` are the current embedded application checks.

| Test | Windows MSVC / WebView2 |
| --- | ---: |
| BabelAssetFresh | 120.68 s |
| LifeEmbeddedBuild | 39.59 s |
| DeveloperEmbeddedBuild (retired PI fixture) | 39.20 s |
| AddSubdirectoryConsumer | 26.33 s |
| DeveloperBrowserIntegration | 26.01 s |
| CEFTestingState | 25.06 s |
| FrontendSelection | 20.95 s |

The complete suite took 338.12 s on Windows, 130.94 s on Linux and 108.31 s on
macOS. These suites include different platform tests and build configurations;
their totals should not be compared as a compiler benchmark.

## Changes

- The asset renderer assembles bounded lines (in CMake since #318) and joins them once. The
  former CMake loop repeatedly processed a growing string for every 64-bit word.
  All checked-in headers, compression settings and full byte comparisons remain
  the same. No intermediate gzip file is needed. The independent zlib decode
  check and every per-asset freshness test still run.
- Compiler-free frontend and CEF fixtures reuse the parent generator, avoiding
  repeated host-default Visual Studio discovery in Ninja CI builds on Windows.
  The focused Ninja / Ninja Multi-Config / Ninja switching test is retained.
- The consumer build reads the actual test target's compiler cache launcher at
  generation time, instead of depending on a configure-time variable or
  independently discovering a different cache. It still configures,
  cleans, compiles and links the real consumer on every invocation.
- Independent tests overlap under CTest. The remaining Life embedded build retains
  `RUN_SERIAL` so a CMake regeneration or shared relink cannot race a running
  binary in the same tree, including on Windows. A build-only resource lock was
  insufficient for that guarantee.
- The developer bootstrap unit test uses an ephemeral port even without a project
  directory. Its former default-port bind collided with browser tests in the first
  macOS CI run; reserving port 9002 reproduces that failure deterministically.
- Filesystem fixtures share the atomic temporary-directory owner instead of
  picking one of only 9,000 paths without checking exclusive creation.
- Asset generation and checking resolve the entry script's real path, so a checkout
  reached through a symbolic link still executes the CLI. A CMake regression covers
  stale checks and generation through a directory link (a junction on Windows), for
  both identity and gzip assets.
- The attribution regression retains all assertions and parallel scheduling, with
  a 180-second timeout. Its many short Git/grep subprocesses exceeded the previous
  60-second budget under contention in the corrected-head
  [macOS job](https://github.com/ambroise-leclerc/WebFront/actions/runs/39674764032/job/119481980751)
  (61.46 seconds before CTest stopped it).

The four-second `WfDriverTimeout` case intentionally exercises the real driver watchdog. A tiny compatible
environment starts a sleeping command and publishes a readiness marker; the test requires that marker as well as
exit status 71. Compiler startup is covered by the other real-script driver cases and does not consume this watchdog's
budget. This case retains a real elapsed-time check rather than advancing a test clock.

## Developer journey (#316)

The 15 `Journey*` cases (create, init, install, check, test, build, embed, build and run of the embedded application, and the
error cases) run one after the other on a single project. Their summed CTest durations in the
[pull request run](https://github.com/ambroise-leclerc/WebFront/actions/runs/40081726319) and the
[CEF dispatch](https://github.com/ambroise-leclerc/WebFront/actions/runs/40081723204), and the whole suite in the same run:

| Job | Journey cases | Whole suite |
| --- | ---: | ---: |
| Linux GCC / WebKitGTK | 64.1 s | 248.7 s |
| macOS Clang / WKWebView | 60.8 s | 254.5 s |
| Windows MSVC / WebView2 | 73.3 s | 269.4 s |
| Linux GCC / CEF | 44.8 s | 172.7 s |
| Linux Clang / CEF | 44.9 s | 170.4 s |

The cases run under the same lock as the other browser tests, so the added time is about one minute per job, of which the
compilation of the embedded application (`JourneyEmbeddedBuild`, serial like `LifeEmbeddedBuild`) is the largest part. The project of the
journey sits under `<build>/jp`, a short root kept from before the developer store handled paths beyond 260 characters on Windows (#349).

## Resource audit

| Tests | Scheduling constraint |
| --- | --- |
| Catch2 scenarios | Separate processes; mutable globals and current directories are process-local. Filesystem fixtures atomically claim temporary directories; the one relative `test.tmp` is used by one scenario only. Real socket tests use ephemeral ports, including developer bootstrap without a project. |
| Asset checks | Read sources and compare full headers in memory; no shared temporary gzip/header output. |
| JavaScript and attribution regressions | Own temporary directories/repositories. |
| Frontend selection | Its own `frontend-tests` directory; multi-config tests have a separate working directory. Each configure starts with a fresh fixture cache. |
| CEF option scope | Its own `cef-testing-state` directory; each state still configures separately. |
| Consumer | Separate `consumer-build` tree; no browser is opened. |
| Journey | One project directory (`<build>/jp`) chained by CTest fixtures; the browser lock; the nested build is `RUN_SERIAL`. |
| Browser integration and lifecycle | Keep `RESOURCE_LOCK webfront_port_9002`, including the shared CEF profile and generated browser resources. |
| Life embedded build | Keep `FIXTURES_REQUIRED DeveloperGenerated`, then the build/run fixture dependencies. `RUN_SERIAL TRUE` excludes every other test while it runs, protecting running executables from regeneration/relinking. `PROCESSORS 2` records the nested build's two workers. |

The browser setup must finish generating headers before the Life embedded build
starts. Browser tests remain mutually exclusive. No test runs alongside the nested build. The consumer has a different
build tree; ordinary independent tests can overlap when the embedded build
is not running.

Linux coverage processes still write GCC's normal `.gcda` files on the runner's
local filesystem, using libgcov's file locking. Do not copy this parallel coverage
command to a filesystem without locking support without isolating and merging
the profiles. Sanitizer test processes retain their existing runtime settings.

## Local measurements

Measured on the same Linux workstation with GCC 16.1, CMake 4.2.3 and Release builds. (The Node.js
renderer measured here has since been replaced by the CMake one.) These are local measurements, not predicted hosted-runner
results. CEF compilation was also active during these measurements.

| Scope | Before | After |
| --- | ---: | ---: |
| Babel freshness command, original vs new renderer | 14.75 s | 0.30 s |
| Same 22 asset/frontend/CEF checks, serial | 24.73 s | 9.40 s |
| Complete 261-test CEF-off suite, new renderer, serial vs four workers | 36.78 s | 18.13–18.80 s |

The full-suite row isolates scheduling with the optimized renderer already in
both runs; it is not a measurement of the original full suite. The new generator
regression is included. All runs check actual regenerated bytes and error paths.

Before review, the 14-test CEF/Xvfb integration suite passed twice (56.28 s and
53.28 s) with the build-only lock. That observation did not establish that shared
relinking was safe on Windows; it has been replaced by whole-tree serialization.
The first run's tests over 20 s were DeveloperEmbeddedBuild (22.25 s) and
LifeEmbeddedBuild (22.38 s): each compiles a different generated provider and
links/copies the real CEF runtime. Local compilation used no compiler cache;
the CI launcher is preserved for these targets.

After review, all 14 CEF/Xvfb tests pass with restored whole-tree serialization
(63.18 s). No browser or unit process starts alongside either nested build.
The corrected 261-test CEF-off suite passes with four workers in 18.02 s.
Repeating each of those 261 cases three times also passes (54.16 s total).

The first [PR run at 65817bf](https://github.com/ambroise-leclerc/WebFront/actions/runs/39673110800)
completed Linux's covered native-browser suite in 47.63 s, down from 130.94 s
in the baseline. Coverage collection and upload succeeded. macOS took 77.38 s
versus 108.31 s, but failed the developer bootstrap test because its default port
collided with a browser. That run is not a successful macOS qualification. The
unit test now requests port 0, and nested builds have whole-tree exclusion.

The Windows target of at least 50% less than 338.12 s, hosted macOS success and
repeated-run stability must be confirmed on the corrected head. Local Linux results
do not qualify those platforms. Use the retained reports to record runner, build
configuration, compiler, cache state and parallelism for each comparison.

## Remaining costs and reproduction

Embedded builds compile actual browser-generated headers and link the frontend;
they must remain after browser setup. Their existing compiler cache can reduce
repeated work, but a cold run still pays compilation/linking and runtime copying.
The consumer also deliberately exercises a clean compile. Browser development
integration runs actual dependency resolution, compilation, previews and export;
its dependent stages cannot simply be made independent CTest processes. Future
profiling should distinguish those stages before changing the scenario.

After configuring and building dependencies, run offline from the checkout:

```sh
ctest --test-dir build --output-on-failure --parallel 1 --output-junit "$PWD/build/serial.xml"
ctest --test-dir build --output-on-failure --parallel 4 --output-junit "$PWD/build/parallel.xml"
ctest --test-dir build --output-on-failure --parallel 4 --repeat until-fail:3
ctest --test-dir build-cef -L web-integration --output-on-failure --parallel 4
```

Compare both the total real elapsed time printed by CTest and the individual
JUnit `testcase` durations; summing parallel test times overstates wall time.
For browser/build comparisons, record cold and warm compilation separately.
To repeat every case on the three hosted desktop platforms while also running the
extended CEF/sanitizer checks:

```sh
gh workflow run BuildAndTest.yml --ref <pr-branch> -f repeat_desktop_tests=true
```

Use the ordinary PR run (one repetition) for before/after performance comparisons;
the three-repeat run is a stability check and its wall time includes repetitions.
Every remaining test over 20 s should be reviewed against the costs above rather
than disabled or given weaker assertions.
