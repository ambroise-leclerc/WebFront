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
rewritten; use a dedicated result directory for each invocation. Without `--suite`, the existing one-command interface
and its exit statuses remain available.

Browser sessions and independent driver cases bind loopback port `0`; `TCPStdnet` atomically binds an OS-assigned port.
`app.port()` and frontend navigation use that actual port, as does the HTTP/WebSocket content policy. Custom network
providers without `acceptor.local_endpoint().port()` retain their previous port forwarding. Each driver workspace,
profile and project has a separate directory. Tests that mutate the same project retain a resource lock and ordered
fixtures. Native lifecycle tests using fixed URLs keep the `webfront_port_9002` lock. CEF driver sessions additionally override Chromium's
root/cache profile; its default beside the executable would otherwise forward a second process into the first browser
singleton even with independent HTTP ports. Nested builds retain that lock
and `webfront_build_tree` to protect shared browser DLLs and build outputs on Windows, while unrelated unit tests can run.

## Visible windows and retained lifecycle contracts

Driver, embedded-distribution and compatible lifecycle windows use `visible=false` on WebView2/WKWebView. They still
create a native window, real engine/controller and native event loop. Window visibility is asserted in the native
lifecycle tests. Failed navigation, close, startup failures, watchdogs and cancellation remain real process tests.
The two macOS executable/dylib registration orders remain separate images and processes, with hidden native windows.

The developer integration keeps its visible interactive terminal and Life preview. The macOS agent-application case
keeps one visible window because its contract asserts visibility and the absence of a Dock icon. The automated direct
transport contract harness retains one native window on each desktop. Linux's real windows are confined to Xvfb. This
budgets **two visible openings on Windows and three on macOS** in the standard configuration; engine-specific manual
benchmarks and diagnostic runs are outside this budget. Restore all diagnostic windows with
`WEBFRONT_TEST_VISIBLE=1`. A hidden WKWebView requires macOS 14+ (public inactive scheduling preference) and a WindowServer session; this is not a headless-engine substitute.

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

`DeveloperBrowserIntegration` emits begin/end timestamps for each major phase and every actual command, including
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
counts and cache state so another run can be interpreted. Historical local profiles remain in Git.

Consumer and embedded tests compile real C++ consumers of public headers. Frontend configuration
checks need independent CMake scopes. Read slow cases with their phase transcripts and these
compilation constraints before changing their coverage.

GCC parallel coverage relies on libgcov's filesystem locking; filesystems without locking require
isolated profiles and merging. macOS failures retain WebKit system/crash diagnostics and phase logs.
