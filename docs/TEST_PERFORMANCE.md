# Test execution and profiling

Run the complete fixture graph, resetting the projects and profiles on every pass:

```sh
cmake -DBUILD_DIR=build -DCONFIG=Release -DREPEAT=3 -DPARALLEL=4 -P test/cmake/RunTestSuite.cmake
```

`PARALLEL` is a positive CTest worker bound (default 4). Start with four test workers and two compiler workers;
raise them only after measuring peak process-family RSS on the actual machine. Nested embedded builds account for two
CTest processors. On Linux this runner owns one dedicated Xvfb display for the entire invocation. Direct `ctest` commands
still get a private display per browser process. Neither mode opens windows on the user's Linux desktop.

## One process per wf command

Every `WfDriver*`, `Journey*`, `SelfBuild*`, `WebSuite*`, `ReactAdapter*` and publication case runs `webfront-wf` once:
one browser, one command, its exit status, output and report checked by `test/cmake/RunWfDriver.cmake`. Filter the
familiar names with `ctest --test-dir build -R WfDriverTestFails --output-on-failure`; CTest adds the fixtures the case
requires. Repetition must repeat the complete fixture graph, rather than `ctest --repeat` on individual stateful commands.

Grouping compatible commands in one browser (`WEBFRONT_TEST_SESSIONS`, #544) was retired by #570. A session reserved four
CTest processors for its window, which serialized commands that otherwise run side by side, and it needed a suite file,
a report per case, a reader per report and a regression of its own failure path. On Windows (MSVC Release, WebView2,
12 threads, four workers, warm caches), the 60 driver cases with their fixtures, measured twice in each mode:

| Mode | Cases | Wall time |
| --- | --- | --- |
| Sessions (eight shared windows) | 70 | 99.4 s, 83.4 s |
| One process per command | 60 | 60.8 s, 62.6 s |

The complete suite without the portability tier then takes 159 s for 169 cases on that machine.

The driver's optional `--command-timeout` starts after bootstrap, just before the command runs; `--timeout` bounds the
whole invocation. `WfDriverTimeout` uses a deliberately slow bootstrap and a four-second command limit, requiring the
sleeping command's start marker as well as native watchdog status 71. The command and its Jasmine runner are displayed
in the native window.

Browser tests and driver cases bind loopback port `0`; `TCPStdnet` atomically binds an OS-assigned port.
`app.port()` and frontend navigation use that actual port, as does the HTTP/WebSocket content policy. Custom network
providers without `acceptor.local_endpoint().port()` retain their previous port forwarding. Each driver workspace,
profile and project has a separate directory. Tests that mutate the same project retain a resource lock and ordered
fixtures. Native lifecycle tests using fixed URLs keep the `webfront_port_9002` lock. CEF driver processes additionally override Chromium's
root/cache profile; its default beside the executable would otherwise forward a second process into the first browser
singleton even with independent HTTP ports. Nested builds retain that lock
and `webfront_build_tree` to protect shared browser DLLs and build outputs on Windows, while unrelated unit tests can run.

Automation uses independent ports and project/temporary directories; browser storage isolation
still depends on the frontend, as documented in [AUTOMATION.md](AUTOMATION.md). Its functional MCP scenario does not
hold `webfront_port_9002`. The two watchdog cases alone share `webfront_automation_watchdog`, limiting
deliberately spinning renderers without blocking unrelated browser work. Their test archive replaces
only the operation body with a marker followed by a synchronous loop; signed dispatch, the native
deadline, shutdown grace and process/endpoint cleanup stay real. A single three-second startup delay
exceeds their two-second operation budget, avoiding repeated long bootstrap/Jasmine waits.
Mutated test archives use gzip level 1 to reduce fixture-generation CPU cost. The CLI restart checks
endpoint reuse and Unicode bootstrap error delivery together; successful builds remain covered by
the functional owned/shared-window scenario.

## Visible windows and retained lifecycle contracts

Browser tests keep their windows visible by default on Windows and macOS. Their rendering is a daily visual check;
`WEBFRONT_TEST_VISIBLE=1` is no longer needed. Reduce repeated openings by reusing a window, rather than hiding it.
Linux's real windows remain on the suite's Xvfb display. Explicit tests of hidden-window behavior still use that option.

The six developer integration parts (`DeveloperBrowserIntegration`, `-scripts`, `-typescript`, `-jasmine`, `-terminal`,
`-workspace`) are six processes, each with its project, origin and visible window, which CTest runs side by side.
The shared window that hosted them as panes (`DeveloperBrowserSession`, #556) was retired by #570, because its gain was
never shown and it failed more often than the parts it replaced:

| Platform | Six processes, four workers | One shared window |
| --- | --- | --- |
| Windows, MSVC Release, WebView2 | 26.89 s | 28.69 s |
| macOS 27, WKWebView, 8 cores | 10.8 to 11.4 s | 10.8 to 11.4 s |

The parts' total is bound by the JavaScript and Worker work of each page, not by the number of windows. Over the 82
completed runs of `BuildAndTest.yml` before #570, `DeveloperBrowserSession` and its failure regression failed 7 times
on macOS and Linux, and the session needed a report per part, a reader per report, a nested CTest regression and a
native deadline that wrote the reports of the parts it interrupted.

Failed navigation, native close, startup failures, watchdogs and cancellation remain real process tests. The two macOS
executable/dylib registration orders remain separate images and processes. `ConsumerJasmine` modes and `Automation*`
retain their own visible windows; no whole-suite budget of two or three visible openings is claimed. Hidden WKWebView
tests require macOS 14+ and a WindowServer session, and check both native invisibility and
`inactiveSchedulingPolicy == None` (the documented rejection on macOS 13.3, which lacks that preference).
The timed `ConsumerJasmine-capture` sequence reserves four CTest processors, so another test cannot cover its
visible window and throttle WebKit's rendering while it records.

## The WebView2 early-close failure (#564)

`WebView2Lifecycle-native-early-close` posts a native close while the window is being created, twice in one process,
and required the thread's COM apartment to be released after each `show()`. It was the most frequent failure of the
suite: 13 of the 82 completed runs before #570. On a 12-thread Windows 11 machine with runtime 157.0.4341.46:

| Condition | Failures |
| --- | --- |
| Idle machine, 30 runs | 0 |
| 16 busy threads, 40 runs | 29, always at the second `show()` |
| Same load, `repeated-show` (six windows that run to their end), 6 runs | 0 |

In passing and failing runs alike, the environment is created, the controller creation is started, the close arrives
and the creation completes with `E_ABORT` while the window is destroyed. Exactly one COM initialization then remains on
the thread, which five seconds of message pumping do not release. Waiting for the controller, then closing it, before
destroying the window leaves the same initialization behind. The frontend's own `CoInitializeEx` and `CoUninitialize`
are paired by `Apartment`: the remaining one belongs to the runtime. The case therefore keeps what the frontend answers
for (the close is honoured, a second window opens, no bridge connects), the released apartment is required after
windows that ran to their end (`repeated-show`), and a unit scenario checks that `Apartment` leaves a thread as it
found it. After that change the case passes 50 of 50 runs under the same load. The limit is listed in
[PROJECT_STATUS.md](PROJECT_STATUS.md#contracts-and-limitations).

## Inventory and intermittent failures

Tracked lines on `develop` before #570 (13 November 2026), fuzz corpus excluded: `test/unit` 24 771, `test/web` 11 990,
`test/cmake` 5 358, `test/tools` 1 581, `test/driver` 882, `test/fuzz` 500. The largest files are
`test/web/developer-specs.mjs` (1 530), `NativeStorageTests.cpp` (1 320), `WebFrontTests.cpp` (1 203),
`MessagesTests.cpp` (1 072), `DeveloperTests.cpp` (1 021) and `test/web/JasmineTest.cpp` (893).

Failed CTest cases of the 82 completed runs of `BuildAndTest.yml` listed by `FlakeReport.py --limit 100` on that date,
31 of which failed. Cases that failed on every platform of one run are regressions of that branch, not flakes, and are
left out:

| Case | Failures | Where | State after #570 |
| --- | --- | --- | --- |
| `WebView2Lifecycle-native-early-close` | 13 | Windows | Explained above; assertion scoped |
| `DeveloperBrowserSession`, `DeveloperBrowserSessionFailure` | 7 | macOS, Linux | Removed with the shared window |
| `DeveloperBrowserIntegration-workspace` | 5 | Windows 4, macOS 1 | Open |
| `Running out of descriptors does not mark the root as moved` | 4 | `gcc13-arm64` | Open |
| `ConsumerJasmine-capture` | 3 | Windows 2, macOS 1 | Open |
| `DeveloperBrowserIntegration-scripts` | 3 | Linux | Open |
| `A large body is streamed without being held` | 3 | `gcc13-arm64` | Open |
| `AutomationConsumerWindow`, `AutomationMcpWindow` | 3 | Windows 2, Linux 1 | Open |
| `JourneyBuildSession` | 1 | Windows | Removed with the driver sessions |
| Two unit scenarios under ThreadSanitizer (fallback logger, streamed reply) | 2 | `sanitizers (thread)` | Open |
| Step `Analyze CTest timings`, no failed case | 3 jobs | all desktops | Step removed |

Durations and coverage per file are not in this table: the JUnit reports of a run give the durations, and the coverage
capture runs on Linux only.

## Reading the durations

The JUnit report of each pass (`build/test-results-<pass>.xml`) holds the duration of every case, and
`ctest --test-dir build --show-only=json-v1` the dependencies and locks. The tools that ranked and sampled them
(`ProfileTests.py`, `MeasureCommand.py`, `CompareCoverage.py`) and their CI steps were removed by #570: no decision
relied on their reports, and the analysis step itself failed three jobs of the last 82 runs. JUnit rounds suite wall
time to whole seconds, and a sum of case durations is not wall time.

`DeveloperBrowserIntegration` and its five sibling parts emit begin/end timestamps for each major phase and every actual command, including
expected failures. `DeveloperSpecs` reports both ends of each spec. The driver reports browser opening/return and shutdown, so process-exit costs remain distinguishable. `LastTest-<pass>.log` keeps the full transcript of each pass; the suite
runner raises the CTest output cap to 2 MiB per case so JUnit retains these details too. WASM compilation/initialization, command preparation, publication, navigation and execution can be
located by the enclosing command and spec; these spans include waits and bridge traffic, not just CPU time.

The esbuild helper used by developer specs initializes its WASM compiler in a Worker, matching
the developer environment and avoiding off-screen frame timer throttling. Its rebuild spec changes
an imported file, checks a syntax error and checks recovery.

## CI, caches, coverage and fuzzers

Windows and macOS start independently of `linux-image`; only Linux waits for its Dockerfile-hashed image. The image
already supplies GCC/WebKitGTK and the fast JSON module. Desktop, CEF and sanitizer
jobs explicitly enable and report sccache. Verified bootstrap and registry archives are cached with keys containing their pin/manifest; configure still checks their integrity. Dependency caches retain their existing keys.
reproducibility outputs are rebuilt rather than restored from a cache.

The coverage step forces `lcov_json_module=JSON::XS`, about eight times faster than the pure-Perl parser. Public header
coverage and the existing exclusions are unchanged. The module selection is part of [lcov's JSON support](https://github.com/linux-test-project/lcov/blob/v2.3.1/lib/lcovutil.pm).

The five fuzzers run with two workers by default, each retaining its 1 GiB RSS limit, its own log, artifact prefix and
writable corpus (including a separate gzip differential corpus). All are awaited and any failure fails the step:

```sh
python3 test/fuzz/RunFuzzers.py build-sanitizers/test/fuzz --jobs 2 --seconds 30
```

`--jobs` is configurable; budget at least 1 GiB per worker plus the runner and sanitizer overhead. CI retains logs and
JUnit reports and prints cache statistics. Qualification dispatches `BuildAndTest.yml` with `repeat_desktop_tests=true`
for `desktop_passes` complete desktop passes (three by default) plus GCC/Clang CEF and sanitizers. First-pass fixtures/profiles are fresh; later passes reset them too but may
benefit from OS/filesystem and compiler caches. They are not measurements of a physically cold OS page cache.

## Investigating intermittent failures

Read the failures of recent workflow runs, including earlier attempts of a run that later passed:

```sh
python3 test/tools/FlakeReport.py --limit 100
```

This read-only tool requires authenticated `gh`. Correlate the case, platform and revision before treating a failure as
intermittent. Repeat the complete fixture graph with `RunTestSuite.cmake` as above; repeating an individual stateful
command does not reset its inputs. Do not add automatic CI retries.

The developer integration executable, `webfront-wf`, the browser suite driver and the automation consumer log HTTP
requests, response statuses and streaming failures to stderr. The agent does so only in a browser test build.
These logs retain warnings and errors and filter out ordinary bridge traffic to keep resource diagnostics within CTest's output cap; service
tokens and request queries retain the HTTP server's redaction. JSON/MCP stdout stays separate.
The workspace reloading assertion also reports the preview URL, document readiness, displayed root, tab status and
`wf dev` output. A 200 response alone does not prove that a frame executed its script.

The native automation supervisor's unit tests advance a steady-clock time source to check the operation/startup
boundary and the close grace separately. They wait for the supervisor to observe each advance, rather than sleeping
through a 1.1-second deadline. A separate case retains the default real clock. Startup readiness and expiry are checked
under one session lock; time spent sending the browser close request counts toward its grace.

Asynchronous unit-test guards use predicates with early exit: capture tests wait for their published file or collected
result, executor tests wait for attachment/closure, and streamed-reply shutdown waits for a registered deferred send.
A scheduling guard does not change a product deadline. Sleeps that deliberately hold a mock operation, exercise a
process timeout or check that a disarm remains blocked are separate from these readiness waits.

The suite runner is the one way to repeat: manual dispatch with `repeat_desktop_tests=true` runs `desktop_passes`
complete desktop graphs (three by default), resetting the fixtures on each pass and stopping at the first failure.
`-f desktop_passes=20` qualifies a reliability fix; more than three passes raise the job budget from 90 to 300 minutes.
A single case is repeated locally with `ctest -R <name> --repeat until-fail:<n>` when it owns no fixture state.

## Recording a run

Write JUnit reports under the build directory.
CI uploads these as workflow artifacts; keep issue-specific before/after observations in the issue
or pull request. Record the revision, platform, compiler, configuration, worker bounds, case/spec
counts and cache state so another run can be interpreted. Historical local profiles and
[the #388 measurement summary](https://github.com/ambroise-leclerc/WebFront/blob/cce1927ec952cb5d80b7bb130e9eaddbd1744885/docs/TEST_PERFORMANCE.md#measurements)
remain available at cce1927, the last revision containing the
[raw reports](https://github.com/ambroise-leclerc/WebFront/tree/cce1927ec952cb5d80b7bb130e9eaddbd1744885/docs/test-performance).

Consumer and embedded tests compile real C++ consumers of public headers. Frontend configuration
checks need independent CMake scopes. Read slow cases with their phase transcripts and these
compilation constraints before changing their coverage.

GCC parallel coverage relies on libgcov's filesystem locking; filesystems without locking require
isolated profiles and merging. macOS failures retain WebKit system/crash diagnostics and phase logs.

Build-time measurements (compiler cache scenarios, grouping, precompiled headers, `/analyze`) and the decisions taken are in
[BUILD_TIME_STUDY.md](BUILD_TIME_STUDY.md).
