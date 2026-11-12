# Test execution and profiling

Run the complete fixture graph, resetting the projects and profiles on every pass:

```sh
cmake -DBUILD_DIR=build -DCONFIG=Release -DREPEAT=3 -DPARALLEL=4 -P test/cmake/RunTestSuite.cmake
```

`PARALLEL` is a positive CTest worker bound (default 4). Start with four test workers and two compiler workers;
raise them only after measuring peak process-family RSS on the actual machine. Nested embedded builds account for two
CTest processors. On Linux this runner owns one dedicated Xvfb display for the entire invocation. Direct `ctest` commands
still get a private display per browser process. Neither mode opens windows on the user's Linux desktop.

Compatible commands use one browser per session and a new HTTP iframe/realm per command. The project is deliberately
shared within each group, in this order:

| Session | Individual CTest assertions retained |
| --- | --- |
| WfDriverSession | install, passing test, failing test, unknown command, script output |
| JourneyStartSession | create, init, install |
| JourneyBuildSession | check, test, build, embed |
| JourneyRestoreSession | missing offline cache, restore, verified offline cache |
| SelfBuildSession | install, build, developer specs, repeat build, permissions refusal |
| WebSuiteSession | install the browser suite, React adapter check and build |
| PublicationSession | React package, services package, site build and repeat |
| WfDriverManifestSession | initialize and install with the manifest in a subdirectory |

The Journey file preparation and cache deletion remain CTest fixtures between sessions. Self-build reproducibility,
refresh, publication and tampering checks retain their own dependencies. The driver writes a JSON result per command;
CTest checks the original exit code, output, structured report and file assertions. Unexpected failures make the session
fail and do not skip subsequent commands. A browser crash, early closure, missing report or watchdog expiry fails the
session. A native regression exercises a mismatch followed by a successful command. No existing scenario is removed.

Each new realm owns its bridge connection, imports, timers, Workers, listeners and child frames; removing it releases
those resources. Local/session storage and enumerated IndexedDB databases are reset before the next command. The native project Store survives intentionally. Pinned and rebuilt environments retain different
profiles and URLs/ports. The normal suite resets both; never point the two revisions at a shared persistent profile.

Filter the familiar CTest names with `ctest --test-dir build -R WfDriverTestFails --output-on-failure`. CTest includes the
session fixture and its prerequisite project setup. For a truly isolated process per case, configure
`-DWEBFRONT_TEST_SESSIONS=OFF` and use the same filter. A case that blocks consumes its session's remaining budget and the later cases then report "No result for the suite case": rerun with sessions off to find the cause. Repetition must repeat the complete fixture graph, rather than
`ctest --repeat` on individual stateful commands.

The driver's session interface also accepts an explicit list and a name substring filter:

```json
[
  {"name":"list","command":["list"],"expect":"0"},
  {"name":"unknown","command":["nosuchcommand"],"expect":"1"}
]
```

```sh
build/test/web/webfront-wf --project /absolute/project --suite /absolute/cases.json \
  --report-dir /absolute/results --filter unknown --timeout 120
```

`timeout` optionally sets the native per-case limit in seconds (a decimal string, default 100). The driver enforces
both the per-case deadline and `--timeout` for the whole session; CTest groups sum their original case budgets and keep
a 20-second outer grace. `expect` is a decimal status string; commands are argument arrays, never shell text. Case names must be unique and use
letters, digits, `_` or `-`. The argument JSON is bounded to 60000 bytes by the bridge. Only the selected reports are
rewritten; use a dedicated result directory for each invocation. Without `--suite`, the one-command interface
and its exit statuses remain available. Its optional `--command-timeout` starts after bootstrap,
just before the command runs; `--timeout` still bounds the whole invocation. `WfDriverTimeout`
uses a deliberately slow bootstrap and a four-second command limit, requiring the sleeping command's
start marker as well as native watchdog status 71.

Browser sessions and independent driver cases bind loopback port `0`; `TCPStdnet` atomically binds an OS-assigned port.
`app.port()` and frontend navigation use that actual port, as does the HTTP/WebSocket content policy. Custom network
providers without `acceptor.local_endpoint().port()` retain their previous port forwarding. Each driver workspace,
profile and project has a separate directory. Tests that mutate the same project retain a resource lock and ordered
fixtures. Native lifecycle tests using fixed URLs keep the `webfront_port_9002` lock. CEF driver sessions additionally override Chromium's
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

`DeveloperBrowserSession` opens **one visible native window for all six developer integration parts**, instead of six.
It displays up to four parts concurrently in visible iframe panes, each with its own native application, temporary
project, assigned loopback port and origin. Bootstraps are
staggered until their environment is ready so the archive unpackers do not compete on WebKit's shared JavaScript thread;
the ready pages' commands run in parallel. Each page learns its host's origin from its `?developer-session=<origin>`
query, not from its referrer.
A completed frame is removed and its slot reused; its Workers, listeners
and bridge go away. The same native view then navigates to workspace as a top-level page for its layout/rendering and
keyboard-focus checks; this retains WebKit's original focus ancestry. No active pane is hidden.
All assertions run in their original single-part mode, including workspace's first-launch/failed-first-build checks.
The familiar `DeveloperBrowserIntegration*` CTest names validate separate native reports. The producer fixture succeeds
when all six reports are complete and agree with the process status, even if some parts failed; each report reader
decides its own result and unlocks only its own dependent tests. A crash, missing/invalid report or inconsistent
process status fails the producer. An expired native deadline writes the reports of the parts first (a part that never
reported fails with the deadline as its diagnostic; the application and terminal parts, whose shutdown checks cannot
have run, are reported failed) and then exits with 1, so the parts that finished keep their results. The `--session`
command itself still exits with 1 when any part fails. A nested CTest regression verifies failed and successful readers
as well as their downstream fixtures, and a session ended by a shortened deadline (`WEBFRONT_TEST_DEVELOPER_DEADLINE`,
in seconds, 180 by default), using the same registration helper as the real suite. Each report retains the part's diagnostic. Life's generated header and the terminal's
cancellation check still come from their respective parts. Browser deadlines start after file preparation. An explicit hidden WKWebView
case checks both native invisibility and `inactiveSchedulingPolicy == None`; ordinary windows remain visible.
When the runtime lacks this preference (macOS 13.3), that case checks the documented rejection of hidden sessions.

The driver groups above each use one visible window. This change reduces the three WebSuite/React commands from three
openings to one, publication's four commands from four to one, and manifest-directory init/install from two to one.
With sessions disabled, the original individual commands and the six separate developer windows remain available.
The session reserves four CTest processors to account for its bounded internal parallelism. Each part retains its own
native 180-second deadline, even while other parts finish. Compare full-suite wall time as well as openings before
claiming a speedup. The session prints each part's `connectedMs` and `reportedMs` and the final
`nativeWindows=1 parts=6 durationMs=...` for timing comparisons.

Where the time goes, on macOS 27 with WKWebView (warm caches, 8 cores): the parts take 1.9 to 4.7 s alone, 19.5 s in all,
and 10.8 to 11.4 s run as six separate processes with four workers or as the shared window. In the shared window the five
concurrent parts end after 7 to 9 s (TypeScript, slowed from 4.7 s by the others, is last) and workspace then needs its own
1.9 s at the top level: it is the sequential tail, of the order of the 1.8 s gap of the Windows comparison below (not measured on Windows). Starting TypeScript first and
admitting a fifth pane moved the total from 11.1 s to 10.8 s on this machine; it was not retained, as the CI of that revision
showed a Linux scripts Worker readiness failure and a macOS hang of the nested regression, and 0.3 s does not justify
an unexplained risk. The parts' total is bound by the shared page's JavaScript and Worker work, not by the number of windows. A part's environment is unpacked on every page load,
so a pane cannot warm workspace's bootstrap for its later top-level page.

A local Windows/MSVC Release/WebView2 comparison used the same executable and fixtures, with four workers and no
concurrent compiler: six separate visible windows took 26.89 s; the shared visible window took 28.69 s (28.34 s inside
the native session). All six parts passed in both runs. This single pair demonstrates comparable elapsed time and five
fewer native openings; it does not establish a whole-suite speedup.

Failed navigation, native close, startup failures, watchdogs and cancellation remain real process tests. The two macOS
executable/dylib registration orders remain separate images and processes. `ConsumerJasmine` modes and `Automation*`
retain their own visible windows where they have not yet been grouped; no whole-suite budget of two or three visible
openings is claimed. Hidden WKWebView tests require macOS 14+ and a WindowServer session.
The timed `ConsumerJasmine-capture` sequence reserves four CTest processors too, so another test cannot cover its
visible window and throttle WebKit's rendering while it records.

## Profiling and interpreting the numbers

```sh
ctest --test-dir build -C Release --show-only=json-v1 > build/ctest-graph.json
python3 test/tools/MeasureCommand.py --output build/suite-resources.json -- \
  cmake -DBUILD_DIR=build -DCONFIG=Release -DREPEAT=3 -DPARALLEL=4 -P test/cmake/RunTestSuite.cmake
python3 test/tools/ProfileTests.py build/ctest-graph.json build/test-results-*.xml \
  --workers 4 --log build/Testing/Temporary/LastTest.log --output build/test-profile.txt
```

Python is only needed for optional resource/graph analysis and the CI fuzzer coordinator; it is not a JavaScript runtime
or package manager. Ordinary builds and the suite runner need no Python. The analyzer also accepts `--ninja-log build/.ninja_log` to rank actual translation-unit/link job durations
(inclusive parallel work, not elapsed build time). It produces the slowest cases,
wall-time median/range across passes and a DOT dependency/lock graph. Solid edges include explicit dependencies and
fixture setup/cleanup; dashed edges identify locks. `dependency_path_seconds` is the longest weighted dependency path;
`lower_bound_seconds` additionally considers worker capacity, locked work and globally serial tests. It is a scheduling
lower bound, not an assertion that locks determine one unique observed critical path. JUnit rounds suite wall time to
whole seconds; the command resource report has finer elapsed time. Sum of case durations is never described as wall time.

The resource sampler records sampled CPU, process count and peak summed RSS of the command's process family on all
three desktop platforms. Short-lived children can be missed, and shared pages count in each process's RSS: these are
sampled bounds, not precise unique-memory or process-creation counts. Pre-existing services such as a separately started
sccache server and its compiler children can fall outside this process family; sccache's own statistics complement the
sampled build report. It preserves the measured command's failure status.

`DeveloperBrowserIntegration` and its four sibling parts emit begin/end timestamps for each major phase and every actual command, including
expected failures. `DeveloperSpecs` reports both ends of each spec. The driver reports browser opening/return and shutdown,
and every suite case, so process-exit costs remain distinguishable. `LastTest-<pass>.log` keeps the full transcript of each pass; the suite
runner raises the CTest output cap to 2 MiB per case so JUnit retains these details too. The analyzer ranks the emitted
phase/spec durations. WASM compilation/initialization, command preparation, publication, navigation and execution can be
located by the enclosing command and spec; these spans include waits and bridge traffic, not just CPU time.

The esbuild helper used by developer specs initializes its WASM compiler in a Worker, matching
the developer environment and avoiding off-screen frame timer throttling. Its rebuild spec changes
an imported file, checks a syntax error and checks recovery.

## CI, caches, coverage and fuzzers

Windows and macOS start independently of `linux-image`; only Linux waits for its Dockerfile-hashed image. The image
already supplies GCC/WebKitGTK and the fast JSON module. It also supplies `/usr/bin/time`. Desktop, CEF and sanitizer
jobs explicitly enable and report sccache. Verified bootstrap and registry archives are cached with keys containing their pin/manifest; configure still checks their integrity. Dependency caches retain their existing keys.
reproducibility outputs are rebuilt rather than restored from a cache.

The coverage step forces `lcov_json_module=JSON::XS`, verifies lcov's own backend diagnostic and separately times capture,
filtering and listing. A manual desktop-repeat run also captures once with JSON::PP and compares every file's line,
function and branch counters against XS before filtering. Public header coverage and the existing exclusions are unchanged.
The module selection is part of [lcov's JSON support](https://github.com/linux-test-project/lcov/blob/v2.3.1/lib/lcovutil.pm).

The five fuzzers run with two workers by default, each retaining its 1 GiB RSS limit, its own log, artifact prefix and
writable corpus (including a separate gzip differential corpus). All are awaited and any failure fails the step:

```sh
python3 test/fuzz/RunFuzzers.py build-sanitizers/test/fuzz --jobs 2 --seconds 30
```

`--jobs` is configurable; budget at least 1 GiB per worker plus the runner and sanitizer overhead. CI retains logs and
reports on failure and uploads CTest profiles, dependency graphs, sampled resources, lcov phase measurements and cache
statistics. Qualification dispatches `BuildAndTest.yml` with `repeat_desktop_tests=true` for three complete desktop
passes plus GCC/Clang CEF and sanitizers. First-pass fixtures/profiles are fresh; later passes reset them too but may
benefit from OS/filesystem and compiler caches. They are not measurements of a physically cold OS page cache.

## Recording a run

Write profiles, resource samples, JUnit reports and dependency graphs under the build directory.
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
