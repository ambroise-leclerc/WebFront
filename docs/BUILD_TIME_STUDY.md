# Pull-request CI time: measurements and decisions (#435)

This report answers #435. Every number comes from a GitHub-hosted runner, measured by the `Build time study` workflow
(`.github/workflows/BuildStudy.yml`, driven by `test/tools/BuildStudy.py`, tabulated by `test/tools/BuildStudyReport.py`)
or read from the artifacts of `BuildAndTest.yml`. The workflow is a measuring instrument: it gates nothing.

## Method

Each study job configures `tests` the way CI does (Windows: Release, MSVC, WebView2; Linux: Debug, GCC 16, coverage,
WebKitGTK; both with `-DENABLE_TESTING=ON`), with a local-disk sccache, then times clean builds of one target:

| Scenario | Compiler cache | What it models |
|---|---|---|
| `cold` | empty | first build of a branch, or a cache that expired |
| `warm` | complete | a rerun with no source change |
| `header` | complete, but `include/webfront/WebFront.hpp` changed | a pull request that touches the core header (#400, #401, #428) |
| `heavy-test` | complete, but `WebFrontTests.cpp` changed | a pull request that edits one heavy test |
| `light-test` | complete, but `MimeTypeTests.cpp` changed | a pull request that edits one light test |

The workflow is started by hand (`gh workflow run BuildStudy.yml -f variants=base-j4,unity-heavy-b4 -f windows_reps=3`); its
inputs are the variants (empty: all), the repetitions on each system, and whether to time the builds and to profile three
heavy units. A workflow absent from the default branch cannot be dispatched, so it is available once this change is merged.

A cell is wall seconds. Linux cells are stable (two runs of the same variant: 170 and 172 s cold). **Hosted Windows runners are
not**: the same `base-j4` configuration took 725 s and 466 s cold in two runs. Windows variants are therefore repeated three
times and reported as a median with its range; a Windows difference smaller than that range is not a result.

## Where the compile time goes

Heavy units are the ones that instantiate a full application (`BasicWF` on stdnet with a frontend). Even the smallest one,
`HelloWorld.cpp`, costs 61 s on MSVC.

| Compiler | Unit | Total | Parse of headers | Template instantiation | Optimise and generate |
|---|---|---|---|---|---|
| GCC 16, Debug + coverage | `HelloWorld.cpp` | 11.2 s | 21 % | 23 % | 49 % |
| GCC 16, Debug + coverage | `WebFrontTests.cpp` | 22.1 s | 12 % | 22 % | 56 % |
| Apple Clang, Release | `HelloWorld.cpp` | 21.6 s | frontend 11.9 s (sources 5.5 s) | `BasicWF<TCPStdnet, ...>` 8.5 s inclusive | backend 9.6 s |
| Apple Clang, Release | `WebFrontTests.cpp` | 53.2 s | frontend 19.7 s | mock `BasicWF` instantiations 6 to 9 s each | backend 33.1 s |
| MSVC, Release | `HelloWorld.cpp` | 61 s | `WebFront.hpp` alone 8.2 s | | |

On MSVC the `/d1reportTime` report is printed twice, once per front-end pass; the `/analyze` section below measures what that costs.

Across the translation units of `tests` (Clang `-ftime-trace`, 1,786 s of compile), identical `webfront::` and `stdexec::`
function instantiations that appear in more than one unit account for at most 165 s (9 %, an upper bound: nested
instantiations are counted at each level). That bounds what any sharing of instantiations could save.

## Results

### Parallelism: `--parallel 2` or `--parallel 4`

Cold wall seconds. Windows: median and [range] of three runs; Linux: every run (a hosted Linux runner also varies, by up to
1.5x for `all`).

| Target | Linux `-j2` | Linux `-j4` | Windows `-j2` | Windows `-j4` |
|---|---|---|---|---|
| `tests` | 285, 275 | 170, 172, 138, 181, 224 | 822 [757-828] | 640 [537-725] |
| `all` (what CI builds) | 515, 522 | 255, 376 | 1240 [1213-1253] | 1076 [786-1086] |

`-j4` shortens every build: about 40 % on Linux, 13 to 22 % on Windows, where the runner spread is as wide as the gain. The
peak resident set of the whole process family stays under 1 GB (996 MB for `all` on Windows), well inside the 16 GB of a
hosted runner. Adopted (`BuildAndTest.yml`, desktop jobs).

### Grouping the heavy units of `tests`

`WEBFRONT_TEST_UNITY_BATCH=N` with `WEBFRONT_TEST_UNITY_HEAVY=ON` compiles the 13 heavy units of `tests` N to a file and
leaves the 33 other test files alone (the optional ones
enabled by a configuration come on top). The `header`, `heavy-test` and `light-test` columns are the cost of a pull request that
changes `WebFront.hpp`, `WebFrontTests.cpp` or `MimeTypeTests.cpp` with a complete cache.

Windows (median [range] of three runs, `-j4`):

| Variant | cold | warm | header | heavy-test | light-test | peak RSS MB |
|---|---|---|---|---|---|---|
| no grouping | 640 [537-725] | 32 | 298 [252-339] | 102 [87-115] | 33 | 858 |
| groups of 4 | **355 [334-576]** | 18 | **111 [104-182]** | **74 [69-118]** | 19 | 569 |
| groups of 12 | 501 [297-511] | 29 | 166 [111-166] | 163 [113-169] | 30 | **460** |

Linux (two runs, `-j4`):

| Variant | cold | warm | header | heavy-test | light-test | peak RSS MB |
|---|---|---|---|---|---|---|
| no grouping | 202 [181-224] | 6 | 88 [80-95] | 43 [41-44] | 7 | 704 |
| groups of 4 | **143 [102-185]** | 4 | **45 [35-54]** | **40 [32-48]** | 6 | 553 |
| groups of 6 | 172 [169-174] | 5 | 57 [57-58] | 56 [56-57] | 6 | 518 |
| groups of 12 | 166 [162-171] | 5 | 76 [75-78] | 77 [74-80] | 6 | 506 |

Groups of 4 win on both compilers in every scenario: the unit of work stays small enough to spread over four jobs, and a
change to one heavy test recompiles at most three others. Groups of 12 put every heavy test behind one compilation, which is
slowest when one of them changes. Grouping every unit instead of the heavy ones does not compile (`GzipExecutorTests.cpp`,
`DeveloperTests.cpp` and others define the same names); the heavy units needed five changes (`detail::` made
ambiguous by `using namespace webfront`, a local alias that shadowed a file-scope one, two helpers of `BroadcastTests.cpp`
named like those of `StdnetShutdownTests.cpp`, and a header without `#pragma once`) and the whole suite passes. Adopted for the desktop CI jobs only (`BuildAndTest.yml`): the default build
keeps one object per test file, and the portability and sanitizer jobs still compile each file alone, which would expose
a clash that grouping hides.

### Precompiled headers

`WEBFRONT_TEST_PCH=ON` precompiles Catch2 and `WebFront.hpp` for the heavy units.

| | Windows cold | Windows warm | Windows header | Linux cold | Linux header |
|---|---|---|---|---|---|
| no grouping, no PCH | 640 | 32 | 298 | 202 | 88 |
| PCH | 592 [591-601] | **231** | 227 | 215 | 96 |

sccache cannot cache an MSVC compilation that uses a precompiled header, so every PCH unit is recompiled even with a complete
cache (warm 231 s instead of 32 s) and the build uses 1.7 GB. On GCC it is slower than no PCH. **Not adopted.**

### MSVC `/analyze`

`cmake/CompilerWarnings.cmake` compiles every unit with `/analyze`. `-DWEBFRONT_MSVC_ANALYZE=OFF` (the default is ON) measures
its cost on `tests` with `-j4`:

| | cold | header | heavy-test |
|---|---|---|---|
| `/analyze` (CI) | 640 [537-725] | 298 [252-339] | 102 [87-115] |
| without | 497 [474-517] | 230 [225-231] | 80 [80-82] |
| groups of 4, without | 263 [259-420] | 82 [80-133] | 49 [48-83] |

The analysis costs about 22 % of a Windows build. It is a quality gate (warnings are errors), so removing it from the
desktop job is a decision for the maintainers, not a build-time tweak. One way to keep the gate and the speed is to run it
in the portability job on pull requests into `master` only. **Moved to the master tier** (see "What is left"); the option stays for measuring.

### The pull request that touches a core header

The pull request's own run after merging `develop` (which changes `WebFront.hpp`, the situation of #400, #401 and #428), with
the changes of this pull request in place (`-j4`, heavy units in groups of four), against run 41134609687 on the same kind of
change:

| | Linux | Windows |
|---|---|---|
| Build step | 328 s -> 85 s | 876 s -> 213 s |
| Job | 12 min -> 6.8 min | about 20 min -> 7.7 min |

A fully cached build takes 23 s (Linux) and 33 s (Windows) on this branch, so these builds did recompile the heavy units. The
runs are single samples on runners that vary; the study tables above are the evidence, this is the check that they carry
over to a real pull request.

### What the pull request gains

Test suite wall time on the pull request's own CI against run 41134609687 (same jobs, the compiler cache warm in both):

| | Linux | Windows |
|---|---|---|
| Suite wall time | 256 s -> 174 s | 272 s -> 106 s |
| Worker time (sum of cases) | 869 s -> 576 s | 753 s -> 405 s |
| Dependency chain | 161 s -> 169 s | 143 s -> 62 s |

The Linux gain is the 180 s of `ConsumerJasmine-*` that was waiting for nothing. The Windows chain shortened because
`LifeEmbeddedBuild` no longer waits behind the port lock and, this time, hit the compiler cache; part of the Windows gain is
therefore a faster runner and warm caches, and only the Linux figure is a clean before/after.


## Run time

The suite takes 256 s on Linux and 271 s on Windows (run 41134609687). Measured from `ctest-graph.json` and the JUnit timings:

- **The dependency chain** is `SelfBuildSession` -> `DeveloperFixtures` -> `DeveloperBrowserIntegration` -> `LifeEmbeddedBuild`
  -> `LifeEmbeddedIntegration` (161 s on Linux, 143 s on Windows). `DeveloperFixtures` assembles the environment that
  `SelfBuildSession` builds from the current sources and `DeveloperBrowserIntegration` runs it: the order is the point of
  the tests, so the fixtures cannot be shared to shorten it. `DeveloperBrowserIntegration` alone is 59 to 113 s; most of
  it is one test process (`terminal and sessions` 32 s, `Life preview and bridge` 21 s, `Jasmine runs` 17 s on Linux), so
  splitting it into independent CTest cases is the only way to shorten the chain further.
- **The port lock** `webfront_port_9002` serialised 175 s (Linux) and 203 s (Windows) of tests, which is why the suite did
  not scale with the number of workers. 22 and 23 cases hold it. Two of them, `JourneyEmbeddedBuild` and
  `LifeEmbeddedBuild`, only compile (48 s each on Windows, uncached) and bind no port; they still held it. They now hold
  `webfront_build_tree` alone.
- **`ConsumerJasmine-*` took 30.0 s each on Linux** (six cases, 180 s of worker time) for no work: `RunWithDeadline.sh`
  left a watchdog `sleep` holding the output pipe, so `execute_process` returned only when the deadline expired. Under
  dash, which `/bin/sh` is on Ubuntu, `set -m` succeeds without enabling job control, so the group kill of the same script
  also reached nothing. The script now returns when the command does and gives the command a process group with
  `setsid`. `RunWithDeadline` is the regression test.
- **Consumer projects** (`FrontendConsumerConfiguration` 67 s, `AddSubdirectoryNativeConsumer` 64 s on Windows) configure and
  build separate projects on purpose: each case proves a different option set from a clean state. They are off the
  dependency path, and with the suite bounded by its lock and chain rather than by its workers, parallelising them would not
  shorten the wall time.

## What was not adopted, and why

- **Sharing objects between executables.** `life_journey` and `life_embedded` compile the same `LifeEmbeddedTest.cpp`
  against two different generated asset headers; the filesystem type is a template argument of the application, so the two
  objects differ. The developer application is instantiated with a different frontend or filesystem type in
  `webfront-agent`, `webfront-wf`, `developer_integration` and `DeveloperTests`. The 9 % ceiling above is what an
  `extern template` scheme could reach, at the price of explicitly instantiating `stdexec` internals.
- **Merging small executables.** `jasmine_consumer`, `frontend_lifecycle` and `webview2_*` each cost 66 to 69 s on MSVC and
  each instantiates its own frontend type, so a merged executable would still instantiate every type once. The CTest cases
  would also lose their separate process, which is what several of them prove (exit status, hang, early close).

## What was left after the first pass, and what was done about it

After the changes above a desktop job spends its time as follows (run 41177164644, a pull request that touches a core header):

| Step | Linux | Windows |
|---|---|---|
| Container start / checkout and CMake | 53 s | 23 s |
| Configure | | 20 s |
| Build | 85 s | 214 s |
| Fast tests | 15 s | 17 s |
| Test suite | 195 s | 163 s |
| Coverage (Linux only) | 42 s | |

The compiler-cache probe after the suite (a clean rebuild of `life_embedded`, `life_journey` and the consumer, run for #388) costs
1 to 2 s, so it is not worth removing.

- **`/analyze` and the examples** (about 22 % of a Windows build, and 61 to 67 s each on MSVC) moved to the master tier. Putting
  them in a parallel job would not have shortened a pull request, which waits for both, so a pull request into `develop` no longer
  builds either; the weekly run, a pull request into `master`, a push to `master` and a manual dispatch with
  `repeat_desktop_tests` do (`FULL_TIER` in `BuildAndTest.yml`, `docs/TEST_TIERS.md`).
- **`DeveloperBrowserIntegration`** (59 to 113 s, the longest case, at the end of the chain `SelfBuildSession` ->
  `DeveloperBrowserIntegration` -> `LifeEmbeddedBuild`) was one 1,061-line script in one browser over one project. It is now
  five parts, selected by `developer_integration --part=<name>`: `app` (starter, build, `wf dev`, Life preview, embedding, and
  the `LifeUI.hpp` that `LifeEmbeddedBuild` compiles), `scripts` (package commands, `wf run`, `wf exec`), `typescript`
  (`wf check`), `jasmine` (`wf test`) and `terminal` (the terminal, sessions and history). Each part creates and initialises its
  own project, installs what it needs and listens on a port the system picks, so none holds `webfront_port_9002`; with no
  `--part` the executable plays them all in one session as before. Locally on macOS (WKWebView) the four cases other than `app`
  take 3 to 6 s each once the environment is built.

### What the cut gained

Pull-request runs of the same jobs, before the cut (run 41177164644) and after it (run 41184083189):

| | Linux before | Linux after | Windows before | Windows after |
|---|---|---|---|---|
| `DeveloperBrowserIntegration` | 113 s | 56 s (`app`), 33, 31, 30 and 17 s for the others | 38 s | 22 s (`app`), 15, 14 and 13 s for the others |
| Dependency chain | 186 s | 130 s | 104 s | 89 s |
| Worker time (sum of cases) | 671 s | 735 s | 636 s | 665 s |
| Suite wall time | 194 s | 195 s | 162 s | 212 s |

The chain is shorter, as intended, and the suite is no faster: with four workers the suite is bound by its worker time divided by
four (735 s / 4 = 184 s on Linux, a lower bound of 184 s against a wall time of 195 s), not by its chain. The parts add worker time
(each repeats the bootstrap, `wf create` and `wf init`, and five browsers contend for four cores), and the Windows wall time moved
by more than the runner-to-runner spread. The cut pays when more cores are available (a larger runner, a developer machine) and
it makes a failing part easier to run alone; on a 4-vCPU hosted runner the only way to shorten the suite further is to remove
worker time. The largest cases on Linux are `AutomationMcpWindow` (62 s), `SelfBuildSession` (59 s), `SelfBuildRefresh` (34 s)
and `JourneyBuildSession` (31 s).

### The self-build session and the reproducibility builds

`SelfBuildSession` (59 s on a loaded Linux runner, 44 s on a quiet machine) is five `wf` commands in one browser session:
install 0.4 s, build the environment 8.5 s, run its 121 JavaScript specs 16.3 s, build it again to prove the bytes equal 11.7 s,
and the refusal of a write outside the permissions 6.4 s. `SelfBuildRefresh` (34 s on Linux) builds a third time, from a refreshed
bootstrap, to prove the checked-in bundles are what `wf` produces. Both extra builds are proofs about the sources of the
environment, so they now run, in a pull request into `develop`, only when that pull request changes one of those sources
(`WEBFRONT_TEST_REPRODUCIBILITY`, `docs/TEST_TIERS.md`); locally this takes `SelfBuildSession` from 44 s to 30 s. The first build
is not skipped: the specs and `DeveloperFixtures` need its output, and caching it by a hash of its sources would save 8.5 s here at the
price of a cache whose validity the tests could no longer check.
