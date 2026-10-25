# WebFront project status

## Current state (#317, epic #303, 2026-10-24)

WebFront is a minimal header-only C++23 core plus a standalone developer environment.

- **Core.** `include/webfront/` holds the C++ library and a closed list of embedded resources (the bridge, the fallback page and
  module, the favicon, the bootstrap page and loader, and the release data), guarded at 150 KiB decoded by `MinimalCoreAssets`
  ([ADR-016](adr/ADR-016-bootstrap-comme-socle.md)). No third-party source is vendored.
- **Developer environment.** Downloaded and verified by `NativeApp::bootstrap("developer")` (0.6.2 published and pinned). `wf init`, `create`, `install`, `restore`, `run`, `exec`, `check`, `test`, `build`, `dev`, `preview` and
  `embed` work on a manifest (`webfront.project.json`) and a lock; TypeScript and TSX are compiled by esbuild-wasm and type-checked
  by `wf check` with the project's locked compiler. The application's own commands join the terminal through `webfront::Commands`.
  See [DEVELOPER_ENVIRONMENT](DEVELOPER_ENVIRONMENT.md) and [UI_COMPONENTS](UI_COMPONENTS.md).
- **No JavaScript runtime or package manager** is needed to use, build, test, regenerate, publish or run the CI of WebFront
  ([migration record](NODE_TOOLS_MIGRATION.md)); `NoNodeDependency` audits every tracked file.
- **Proofs.** The `Journey*` chain (create, init, install, check, test, build, embed, compile, run, permission denied, offline cache)
  passes on CEF, WebKitGTK, WKWebView and WebView2 (see below).
- **Limitations.** There is no sandbox: the downloaded environment and the authorized scripts have the authority of the bridge and
  of the project, and the permissions frame only the `context` API of scripts. Only browser packages install; installation scripts
  and native add-ons are not executed. A synchronous infinite loop in a spec blocks the page hosting `wf test`. File reads fail beyond 260 characters
  on Windows (#349). General DOM abstractions, packaging, accessibility and native-engine deployment remain application work.
  The README walkthrough (Game of Life: console, developer, embedded) uses exactly these commands, and `NewcomerDocsMatchExamples`
  keeps it aligned with the examples.

The sections below are dated records, newest first where the topic is current; earlier entries keep the names and numbers of their time.

## Direct frontend access proposal (#363, 2026-10-24)

[ADR-017](adr/ADR-017-acces-direct-aux-frontends.md) is **proposed**: an embedded frontend may later carry the ADR-003 messages and
serve the composed filesystem through its engine's own API (for Android, iOS and other mobile platforms where a loopback server
is awkward), while HTTP/WebSocket remains the normative path of every frontend, of the system browser and of future
microcontroller targets. Nothing of this transport is implemented yet; the ADR lists its acceptance criteria.

`WebKitGTKTransportBenchmark` measures both paths in one WebKitGTK window. The 2026-10-25 Linux measurements
(Ryzen 9 3900X, GCC 16.1, Release without coverage, WebKitGTK 2.52.6, Xvfb) use the same 8000 calls (800 at 32 KiB),
warm-up and five samples as Windows, with the median of three launches; [raw reports](benchmarks/linux-webkitgtk-2026-10-25.json)
are retained. Concurrent throughput is 2.0 to 2.3 times higher through the direct channel; the figures exclude the codec
and thread hop a real transport adds. A dedicated `performance-linux` CI lane repeats the optimized benchmark three times
and retains JSON, CTest and machine reports, alongside the existing Debug/coverage desktop lane.
On WebView2 (Windows 11, Evergreen 155) string messages are slower than the HTML bridge from 32 KiB and for C++ to JS,
while a shared buffer is 2 to 6 times faster. CEF and mobile engines are not measured; the ADR recommends a
direct default for Android and iOS (with version-dependent network rules) and HTML until proven for desktop engines.
The benchmark exposed two per-message diagnostics costs in the bridge, now fixed with regression tests (32 KiB echo: 2.4 ms to 0.7 ms).

The macOS follow-up (2026-10-25) adds `WKWebViewTransportBenchmark`: the same echoes through the HTML bridge, public
message handlers, `evaluateJavaScript`, `callAsyncJavaScript` and a base64 byte envelope. `WKWebViewTransportContracts`
passes native replies/errors, abandoned replies, Unicode/NUL and binary bytes, Promise results, wrong echoes and subframe
rejection. ADR-017 documents the Foundation value boundary and the changes needed for custom-scheme origins and loading;
the benchmark itself still serves its page over HTTP. The allocation-counting test is excluded only from TSan builds
because its global operator replacements conflict with Clang's TSan runtime; ordinary and ASan/UBSan builds keep it.
On the Apple M2 reference host, median 32 KiB sequential echoes through the HTML bridge take 338 µs (JS to C++) and
334 µs (C++ to JS), versus 388 and 393 µs with the base64 byte envelope. Native strings alone hide that cost. The three
reports are retained with their dispersion (the first launch was much slower); macOS keeps the HTML default pending
measurements of a complete transport under controlled load. These figures do not establish iOS performance.

## Repository layout (#320, epic #303)

The root now holds only `.github`, `cmake`, `docs`, `examples`, `include`, `js` and `test`, plus tool configuration files
(`third_party/` was removed by #319: no third-party source is vendored). Headers moved to `include/webfront/` and are included
as `<webfront/WebFront.hpp>`; the former `<WebFront.hpp>`, `<http/...>` or `<system/...>` spellings are
removed without redirect headers (see `docs/CHANGELOG.md`). Browser sources live in `js/core/`
(bridge, fallback, bootstrap loader and embedding modules) and `js/developer/` (environment, terminal
components, build tools, vendoring tools); tests live in `test/{unit,web,fuzz,fixtures,cmake}`. The CTest
case `RepositoryLayout` enforces the root. The generated-provider include changed, so the developer
distribution is bumped to 0.4.1, published and archived as `developer-v0.4.1`. Version 0.5.0 (project manifest, `wf restore`,
`wf run`/`wf exec`, `wf check`, the complete terminal) was published as `developer-v0.5.0`; 0.6.0 (`wf build`, `wf dev`, `wf test`, the non-interactive entry) was published as `developer-v0.6.0`, and 0.6.1 (the output of a script reaches `webfront-wf`) is pinned by `DeveloperReleaseData.hpp`, published and archived as `developer-v0.6.1`. On 2026-10-23 Pages returned HTTP 200 for 0.6.1,
CORS `*` and archives byte-identical to a reproducible local build; the live `--public` test (WebKitGTK, Linux) downloaded them,
fetched React from npm, ran the PI and Life previews and generated both providers.

## Browser suite as a WebFront project (#311, epic #303)

The Jasmine bridge suite is `test/web/suite/`, a WebFront project (`webfront.project.json`: `jasmine-core`, `react`,
`react-dom`, `test.specs`). `webtest` is the `webfront-wf` driver (`test/driver/WfDriver.hpp`) plus the C++ callbacks the
specs call: CTest prepares a copy of the project (`WebSuiteProject`), `wf install` resolves its dependencies from the
local registry built by `wf run test-registry` (`WebSuiteInstall`), and the `Web*Integration` case runs `wf test` in the
pinned 0.6.2 bootstrap environment under CEF/Xvfb, WebKitGTK, WebView2 or WKWebView. `SpecRunner.html`, the `TestFS`
composition and the use of the embedded Jasmine, React and Babel providers by the browser test are gone.
The retired Signal Studio page is no longer part of the suite; Life covers React/TSX in the developer integration. The specs now run
in the hidden frame of `wf test`, so the URL assertions of the former top-level page (path, query, fragment) are covered
by the `path-encoding` lifecycle case, and `webtest` also checks that C++ observed every call the specs make. The Jasmine
case needs no prepared fixtures: configure downloads the registry's packages (#318). The embedded Jasmine, React and Babel providers, their headers (~2.1 MB), `third_party/{babel,react,jasmine}`, `vendor/update.mjs` and `vendor/check.mjs`, their freshness tests and `EmbeddedGzipContents` were removed by #312 (migration: `wf install` and `wf embed`, see `docs/EMBEDDED_ASSETS.md`); the HTTP gzip and identity negotiation tests use a neutral fixture module.

## Self-hosted developer environment (#315, epic #303)

The repository is a `wf` project (`js/developer/webfront.project.json` and `js/developer/webfront-packages.lock.json`, selected by the driver's
`--manifest-dir js/developer`, so that the root stays short). The pinned 0.6.2 bootstrap environment runs `js/developer/scripts/build.mjs` with `webfront-wf`:
the locked esbuild-wasm bundles the environment in the script's Worker, and `environment.tgz`, the starters,
`provision.json`, the pinned header and (with `--refresh-bootstrap`) the bootstrap bundles and their checksums come out
the same twice in a row, and byte for byte as the 0.6.2 that is published (`sha512-SYnS6iRd...`). `site.mjs` stages the Pages
site and refuses to change a published version; `pin.mjs` checks the pin against the published archive; `pages.sh` and
`archive-release.sh` do the host operations with `gh`. The `DeveloperDistribution.yml` jobs `wf-package` and `publish`, and
the actions `wf-driver` and `remove-node`, run only on GitHub.

## Developer journey qualification (#316, epic #303)

The whole journey of a developer is a chain of CTest cases (`Journey*`, label `web-integration`) on one project in an empty
directory, from the environment built from the sources and the local registry, without Node.js, npm or public network after configure
(the CEF job cuts it):

| Step | Case | Proof |
| --- | --- | --- |
| `wf create life` | `JourneyCreate` | the pinned starter is verified and written to `ui/` |
| `wf init` | `JourneyInit` | the manifest |
| `wf install` | `JourneyInstall` | eight packages from the registry, among them `@types/react` (archive rooted at `react/`, not `package/`) |
| `wf check` | `JourneyCheck` | the real Life starter type-checks under TypeScript 5.9 strict |
| `wf test` | `JourneyTest` | two Jasmine specs in the real engine |
| `wf build`, `wf embed` | `JourneyBuild`, `JourneyEmbed` | the header `generated/LifeUI.hpp` |
| compile and run | `JourneyEmbeddedBuild`, `JourneyEmbeddedRun` | the header is compiled into the Life application, which runs from an empty directory |
| `wf preview`, `wf dev` | `DeveloperBrowserIntegration` | they need the page of the terminal: run in the same engine, with Ctrl+C and the cache |

Error cases: wrong integrity, missing archive, unknown protocol or API version and watchdog (`WfDriverWrongIntegrity`,
`WfDriverMissingArchive`, `WfDriverIncompatibleProtocol`, `WfDriverIncompatibleApi`, `WfDriverTimeout`), a script outside its
permissions (`JourneyPermissionDenied`), an emptied cache checked offline then restored (`JourneyOfflineMissing`, `JourneyRestore`,
`JourneyOfflineVerified`), Ctrl+C on dev, scripts and native commands (`DeveloperBrowserIntegration`).

Green on CEF (GCC and Clang, public network cut), WebKitGTK, WKWebView and WebView2 in the
[pull request run](https://github.com/ambroise-leclerc/WebFront/actions/runs/40081726319) and the
[dispatch](https://github.com/ambroise-leclerc/WebFront/actions/runs/40081723204); the durations are in `docs/TEST_PERFORMANCE.md`.
ADR-013, ADR-014 and ADR-015 are accepted on these proofs. The developer store reads and writes project paths beyond 260 characters on Windows through the extended-length form (#349).

The journey found two defects, now fixed with regression specs: the installer refused every archive whose root is not
`package/` (all of `@types/*`, whose root is the package name; `npm` strips the first component whatever it is), and the Life starter
did not type-check under `strict` with the current declarations (`useState<Uint8Array>`).

## Tests, regeneration and CI without Node.js (#318, epic #303)

No path of WebFront depends on a JavaScript runtime or package manager any more: configuring, building, testing, regenerating the embedded assets,
building the environment, publishing it, and the CI.

- **Provisioning.** With `ENABLE_TESTING=ON`, configure downloads the pinned bootstrap environment and the real packages of the
  tests' npm registry (`test/web/registry-packages.json`: thirteen archives, verified against their SHA-512; a wrong digest stops
  the configure; `-DWEBFRONT_BOOTSTRAP_DIR` and `-DWEBFRONT_REGISTRY_DIR` replace the downloads offline). `WEBFRONT_DEVELOPER_FIXTURES`
  is gone: the CTest chain builds the registry (`TestRegistry`: `wf run test-registry`), installs the build dependencies
  from it, builds the environment from the current sources (`SelfBuild*`) and assembles the fixtures of the browser tests
  (`DeveloperFixtures`). At the #344 merge, this rebuilt the published 0.6.1 byte for byte; #335 advances the
  development sources to 0.6.2 while preserving the published 0.6.1 pins.
- **Tests.** The pure JavaScript tests of the former runner became Jasmine specs in `test/developer/specs/`, run by the
  repository's own `wf test` (`DeveloperSpecs`, and the `wf-package` job on the real checkout and lock); 86 tests became 84
  specs because two moved to CMake: the checksums of the bundle inputs (`BundleInputsFresh`, `BundleInputsRejectsChange`) and
  the command names reserved by the terminal and by `webfront::Commands` (`CommandNamesAgree`). The tests of the asset
  generator became `EmbeddedAssetGenerator` (CMake, exit status and format), a Catch2 scenario that decodes every core
  header and compares it with its source, and the per-asset `*AssetFresh` cases. The mapping is in `docs/NODE_TOOLS_MIGRATION.md`.
- **Regeneration.** The headers of the core are rendered, and checked, by `js/core/EmbedAsset.cmake`, in pure CMake (ADR-016):
  they must be checkable by a build that has no browser. `wf run embed-core` is gone.
- **Guard.** `NoNodeDependency` audits every tracked file for imports and APIs of Node, commands of its tools and package
  managers, their configuration, their requirement in current documentation and the "like node.js" positioning; the exceptions are listed,
  with their reason, in `test/cmake/NodeGuardExceptions.cmake`. `NoNodeDependencyRejectsInjections` proves it fails on an
  import of a Node module, a package installation command in a workflow, `engines.node` in a manifest, a Node script nobody calls, an old command in a
  guide and the other injections, and passes on a registry URL and on a test that checks the rejection of a Node import.
- **CI.** The Node.js setup steps, the package installation, the npm cache and the `package` job are gone; the desktop, sanitizer and CEF jobs and
  `wf-package` run `remove-node` (also on Windows and macOS) before anything else.

Verified locally (WebKitGTK, GCC 15) with every JavaScript runtime and package manager absent from `PATH`; see the pull
request for the figures. The workflow changes were checked by running their shell steps locally, not by a workflow run before
the pull request.

## CI test duration (#298, 2026-10-21)

The maintenance asset renderer assembles bounded lines and joins them once, preserving
all versioned bytes. Local Babel freshness dropped from
14.75 s to 0.30 s (the renderer is CMake since #318; Babel is gone). CI uses up to four CTest workers, retains timing reports, locks browser ports
and serializes nested embedded builds against every running test in the same tree.
Frontend fixtures reuse the parent generator, and the consumer inherits the actual
test target's compiler cache launcher at generation time.

The first PR CI run passes Linux coverage/native-browser tests in 47.63 s versus
130.94 s previously. macOS exposes a default-port collision in a developer unit
test, now corrected to use port 0 and reproduced locally with 9002 occupied.
Filesystem fixtures also atomically claim their temporary directories. The corrected
261-test CEF-off suite passes locally with four workers (18.02 s), and all
14 CEF/Xvfb cases pass with serialized nested builds (63.18 s).
Repeating each of the 261 CEF-off cases three times passes locally (54.16 s).
Manual dispatch with `repeat_desktop_tests=true` additionally repeats each desktop
case three times; successful Windows/macOS qualification and stability results must
be checked on the corrected head before merging. See the
[audit, measurements and remaining costs](TEST_PERFORMANCE.md).



## Release 0.4.0 (#200, 2026-10-21)

`webfront::version` and the CMake project version are 0.4.0. The release presents the repository
(examples, README, assets layout) and streamlines CI; it adds no library functionality, see
`docs/CHANGELOG.md`. The developer distribution 0.4.0 is published and archived as `developer-v0.4.0`;
published 0.1.0 to 0.4.0 remain immutable. APIs and packaging remain experimental.

## CI regression scope (2026-10-24)

- Developer environment 0.6.2 is published (`developer-v0.6.2`); the release and test-bootstrap pins name it.
- The retired PI starter, its sources and its distribution archive are removed. Life is the maintained starter and covers
  React/TSX, preview, bitset calls, reproducible embedding and an offline executable without developer callbacks.
  Retired Signal Studio application regressions are also removed.
- Deadline, grace-period and dev-polling checks advance a scoped clock. Worker startup and bridge/UI operations stay
  real. Native command cancellation uses a release handshake; repeated checks reuse immutable fixture package bytes.
- Bounded-snapshot and NodeNext/no-emit compiler checks retain the real compiler in the browser workflow. New
  cancellation and clock specs run with the repository's own `wf test`, preserving #344's self-hosted test pipeline.
  Two identical self-builds demonstrate reproducibility; a separate build verifies bootstrap refresh.
  An end-to-end packaged script verifies publication permissions.
- Tests retain their original CTest grouping: total suite time remains visible. Compilation and complete browser
  workflows are still longer than the two-second target; no timing claim is made by splitting the reporting units.

## Repository presentation for 0.4 (2026-10-21)

- Public examples now live in `examples/`: Hello World and the three stages of Game of Life.
  Browser bridge/component/bootstrap sources live in `js/`; the library remains header-only.
  PI and Signal Studio resources required by regression tests remain fixtures, without public demo targets.
- The README starts with the Life application and its animation, then walks through the native
  bootstrap, dependency installation, development, checks, tests and offline embedding. No library feature is added.
- Per-change CI combines unit tests and native browser coverage into three desktop jobs, with
  npm, CPM and compiler caches. The Linux job prints GCC line coverage on every PR (no external upload). Sanitizers, fuzzing,
  both CEF compiler variants and optional mddlog qualification run weekly or manually. Superseded runs are cancelled.
- Developer distribution 0.4.0 is published for the revised Life text (`developer-v0.4.0`).
- The dated entries below record earlier implementations and their original target names.


Status snapshot: 2026-07-19. Optional mddlog integration note: 2026-10-02. Newcomer-first epic (#205) note: 2026-10-13. UI components note: 2026-10-14. Embedded dependency refresh: 2026-10-19.

## Packed bitsets (#289, 2026-10-20)

- The bridge accepts `std::bitset<N>` callback arguments/results and C++ call arguments plus
  typed future/sender results, using existing short/regular Uint8Array encodings. Bit `i` is
  packed least-significant first into byte `i / 8`; exact lengths and zero padding are validated.
  Zero-sized bitsets are supported. JavaScript must know the logical bit count separately.
- Life registers `nextLife` directly and React exchanges 128 packed bytes per generation.
  Developer distribution 0.3.0 is published on GitHub Pages and archived as `developer-v0.3.0`; the pinned
  0.3.1 (locked reinstall) awaits its `developer-v0.3.1` publication. Published 0.1.0 to 0.3.0 remain immutable.
- Local verification: all 252 CEF-off CTest cases, 25 WKWebView integration/lifecycle cases
  and 14 JavaScript checks pass. Repeated 0.3.0 provisioning produces byte-identical archives
  and bootstrap configuration. Browser tests verify packed Life grids, playback/reset,
  deterministic providers and offline execution from empty working directories.
  CI on [b6bae2e](https://github.com/ambroise-leclerc/WebFront/actions/runs/39490581423)
  passes WKWebView, all 14 Linux CEF/Xvfb cases with GCC, and the 252-case Linux
  thread-sanitized suite. The codec matches the implementation merged in PR #290.
- After reconciling current `develop`, all 253 CEF-off CTest cases and 26 WKWebView
  integration/lifecycle cases pass locally. New developer/embedded test executables use the
  same agent bundles as the shared WKWebView suite; user-facing examples stay ordinary apps.
- The public 0.3.0 environment and both starters passed fresh WKWebView download/build tests.
  Pages returned HTTP 200, CORS `*` and matching pinned bytes. Both public-path providers then
  compiled and ran offline from empty working directories with developer callbacks absent.
- The demo branch uses the codec and browser assertions merged in PR #290, including MSVC's
  empty-bitset guards and future tests that tolerate arbitrary response order. JS cell access
  uses small packed-byte helpers; painting across an octet boundary is covered by the browser test.
- Regression tests cover known bytes, size boundaries, malformed inputs, tuples and both
  browser call directions, including exceptional future/sender completion. Future calls and
  their final notification dispatch on the server context; a joined test thread only waits
  for the results with a ten-second bound. Delayed JavaScript replies arrive out of dispatch
  order, and failures log explicit diagnostics without blocking the server context.
- Local validation on the develop-based branch: all 242 CEF-off CTest cases and the
  WKWebView Jasmine integration test pass. Linux CEF/Xvfb passes with GCC and Clang in
  [CI run 39466294828](https://github.com/ambroise-leclerc/WebFront/actions/runs/39466294828)
  on code revision `d9327e7`; the same run also passes both Windows MSVC configurations,
  the Linux sanitizers, WebKitGTK and macOS/WKWebView checks.


## Historical browser-powered development (2026-10-20)

- `NativeApp` can bootstrap a pinned developer environment with a small embedded loader. The
  downloadable terminal, npm library resolver, esbuild/WASM Worker and gzip/header generator do
  need no JavaScript tooling on application developers' machines.
- The Game of Life demo (`WebFrontLifeDeveloper`, `wf create life`) uses a compact C++23
  bitset simulation and a spatial React/canvas interface with translucent panels, interactive
  patterns, playback, single step and tempo. The original PI demo remains available. Both cover
  local TSX, React/React DOM and scheduler, a same-window preview and an application-owned
  generated provider. Offline executables use those providers without developer roots/callbacks.
  Existing React/Babel/terminal providers remain supported.
- Historical snapshot at the 0.2.0 milestone (the current validation is listed above): local validation passed 250 CEF-off CTest cases, 25 WKWebView integration/lifecycle cases and
  14 JavaScript checks. Browser coverage includes PI results, C++ Life generations, playback/reset,
  deterministic header generation and compiling/running both embedded applications from empty
  working directories. Repeated previews replace session history instead of accumulating entries.
  Linux CEF/Xvfb passes all 14 integration cases with GCC and Clang in
  [run 39421265580](https://github.com/ambroise-leclerc/WebFront/actions/runs/39421265580);
  its WKWebView job also passed. The later Windows and sanitizer results are in the validation above.
- Versioned environment and Life/PI starter archives have a GitHub Pages publication workflow,
  immutable-version checks, dedicated prerelease archiving and a checked-in pinned bootstrap
  configuration. At that time, `wf create life` and `wf create pi` created fresh editable projects without
  overwriting existing files. Maintainer preparation uses `wf` itself (#318); consumers install nothing.
  Version 0.2.0 was served at `https://ambroise-leclerc.github.io/WebFront/developer/0.2.0/`,
  with the same archives retained in the `developer-v0.2.0` prerelease; 0.1.0 and later versions are preserved.
  The live WKWebView smoke test downloaded both starters and the environment from Pages, fetched
  React dependencies from npm, passed PI/Life previews and deterministic generation, then compiled
  those public-path headers and ran both applications offline with developer callbacks absent.
  Pages returned HTTP 200, CORS `*` and byte-identical pinned environment data on 2026-10-20.
- Browser packages only: no installation scripts, native modules, Node API shims or optional
  dependencies. `wf check` (#322) type-checks with the locked `typescript` 5.x/6.x in a Worker, over a bounded
  in-memory snapshot. `wf run`/`wf exec` (#307) run WebFront scripts in a Worker under the
  manifest permissions, which frame the `context` API only and are not a sandbox. Peers require explicit compatible roots/direct
  dependencies. The environment's terminal (#308) runs `wf`, built-in and C++ commands (`webfront::Commands`:
  per-browser coroutine sessions, streamed output, Ctrl+C cancellation without waiting, drained at shutdown) with
  multi-row editing, CJK/emoji widths, persistent history and completion. `wf build` / `wf dev` (#309, environment sources) publish manifest entries and rebuild the preview automatically with a reused esbuild context, bounded input polling, error overlays and Ctrl+C cancellation; `wf test` (#310, environment sources) runs the manifest's specs under the project's locked `jasmine-core` in a hidden frame, with a structured report, exit codes 0/1/124/130 and a non-interactive entry that reports to C++; the release pin is 0.6.0. `webfront-wf` (#321) runs one `wf` command without interaction and returns its status; the bootstrap environment is pinned and verified at configure time (0.6.0, which also contains `wf build`, `wf dev` and `wf test`), and `environment.json` declares the `protocol` and `api` versions that the loader checks. `wf restore [--offline]` (0.5.0, #306) rebuilds or checks
  the cache from the lock, from the application's registry option, else the manifest's, else npmjs.
  See [DEVELOPER_ENVIRONMENT](DEVELOPER_ENVIRONMENT.md) for exact commands and limits.

## Embedded dependency refresh (#284, 2026-10-19)

- React and React DOM 19.3.1, Babel standalone 8.0.7 and Jasmine 7.0.2 replace the older embedded
  versions; xterm 6.0.0 and Fit 0.11.0 remain current. React now uses a local ESM bundle and the
  automatic JSX transform; Jasmine uses its new single boot script. Editable examples still need
  no package installation, CDN or JavaScript build step.
- Dependency JavaScript/CSS uses deterministic gzip generated with the locked fflate 0.8.3;
  sources, licenses, pinned maintenance tooling and offline freshness/decode checks are checked in.
  Identity fallback from #274 also applies to the updated gzip payloads.
- The CEF-off Release build passes all 239 CTest cases after integrating #274. All 20 WKWebView browser/lifecycle cases
  pass locally, including the actual React page, C++ callback recovery and xterm/Fit resizing.
  Linux CEF/Xvfb and Windows WebView2 coverage runs through their existing CI jobs.
- Embedded dependency array storage falls from 1,077,552 to 827,120 bytes. Minimal HelloWorld and
  Greeter binaries were byte-for-byte unchanged in that comparison, before integrating #274.
  See [sources, regeneration and separate gzip /
  upgrade measurements](EMBEDDED_ASSETS.md).


## Release 0.3.0 (#288, 2026-10-21)

`webfront::version` and the CMake project version are 0.3.0. The release adds packed `std::bitset<N>`
bridge values (#289) and the opt-in developer environment with its packed Life demonstration (#288);
see `docs/CHANGELOG.md`. The developer distribution 0.3.1 is published and archived as `developer-v0.3.1`;
published 0.1.0 to 0.3.1 remain immutable. APIs and packaging remain experimental.

## Release 0.2.0 and native frontend epic (#253, 2026-10-19)

All tasks in #253 are merged, including the opt-in platform alias in PR #277.
Compile-time alias checks, CMake selector fixtures and the Windows WebView2,
macOS WKWebView and Linux WebKitGTK browser/lifecycle CI lanes cover the delivered
scope. Existing runtime and packaging limitations remain documented below.

`webfront::version` in `include/webfront/WebFront.hpp` is the version source for CMake.
The C++ version scenario compares the compiled constant with `PROJECT_VERSION`;
`ReleaseVersionConsistency` also checks version tags at HEAD without network access.
The release-version workflow checks the actual tag on tag pushes and published
releases, so a tag such as `v0.1.1` cannot silently disagree with a `0.1.0` header.

The default build remains header-only. `WEBFRONT_USE_MDDLOG=ON` adds a WebFront-owned
compiled diagnostic adapter while keeping consuming translation units module-free.
See [the optional integration contract](mddlog-integration.md) for dependencies,
qualified tools and verification. With the option on, browser logs travel through
mddlog's bounded transport consumer and WebLink captures link/call context. mddlog
ADR-003, which this integration implements, was accepted on 2026-10-02.

## Concurrent spawn and shutdown (#273, 2026-10-19)

Fixed on `develop` by merged [PR #275](https://github.com/ambroise-leclerc/WebFront/pull/275).

`TaskScope::drain()` closes spawn admission and waits for callers already starting
senders before checking whether the context is empty. This fixes the race where
an associated task had not yet started or queued work, so a valid concurrent
`spawn()` caused a false stalled-join exception. Sender startup and cancellation
callbacks run outside the admission mutex. Regression coverage pauses a sender
inside `start()` and also checks that a throwing `connect()` releases admission.
The application-level race test uses an automatically joined thread and reports
shutdown exceptions directly, rather than masking them with `std::terminate`.

## Native macOS window (#258, 2026-10-19)

- `WEBFRONT_EMBED_WKWEBVIEW=ON` enables `WKWebViewFrontend` on macOS 13.3+, using the system
  Cocoa/WebKit frameworks. The implementation remains header-only C++23 through the public
  Objective-C runtime and Clang Blocks; no `.mm` source or engine download is added.
  The system browser remains the default. See [WKWebView notes](WKWEBVIEW.md).
- Main-thread construction/opening, AppKit window/view ownership, resize/focus, Quit/Edit menus,
  native/script closure, navigation failures and an initial loading deadline are handled locally.
  Each opening has a non-persistent website data store. Top-level navigation/redirects use the
  shared content policy; inspection and context menus follow its settings. All popups are blocked,
  including `systemBrowser`, because public WebKit does not expose a reliable user-activation flag.
- The old Jasmine/React/Babel assets now use gzip. WKWebView did not advertise Brotli on local
  HTTP and Jasmine previously stalled after HTTP 506 responses; all decoded payloads were compared
  with the previous embedded versions and are identical. Tests check encoding, gzip signatures and HTTP identity fallback (#274).
  Their combined payloads grow from 504,404 to 723,457 bytes (+43.43%). The server now also
  provides gzip-to-identity fallback (#274); other content codings remain pass-through only.
- Earlier validation on macOS 27 ARM64 with Clang 21.1.8 passed the then-current CEF-off/default
  and WKWebView-enabled suites, including Jasmine, 17 lifecycle cases and two executable/dylib
  runtime-class registration tests. Apple Clang 21 and
  upstream Clang compile the standalone header; an x86_64/macOS 13.3 syntax check also passes.
  A binary linking CEF 154 and WKWebView opens/closes WKWebView successfully. The Jasmine executable
  also passes from temporary `.app` bundles with and without `NSAllowsLocalNetworking` on this host.
- CI adds a dedicated `macos-26` job checking the WindowServer/login session before building
  and running the suite with timeouts. Native Intel/macOS 13.3 runtime execution is not locally
  verified. Linux CEF/Xvfb and Windows WebView2 verification run in their existing CI jobs.
  The application-task spawn/shutdown abort reproduced without WKWebView in
  [#273](https://github.com/ambroise-leclerc/WebFront/issues/273) is fixed by [PR #275](https://github.com/ambroise-leclerc/WebFront/pull/275).

## Optional WebKitGTK frontend (#259, 2026-10-19)

Merged into `develop` in [PR #275](https://github.com/ambroise-leclerc/WebFront/pull/275).

`WEBFRONT_EMBED_WEBKITGTK=ON` enables `frontend::WebKitGTKFrontend` on Linux through
system WebKitGTK 6.0 (GTK 4), discovered by pkg-config. It follows the embedded-window
contract, checks main-thread/display requirements, uses executable-specific XDG data
and cache directories, and loads its runtime without exposing GTK headers. The default
browser selection and disabled configuration remain unchanged. Legacy embedded Jasmine,
React and Babel assets use gzip so WebKitGTK can load them over loopback HTTP. Shared Jasmine and lifecycle coverage,
plus adapter-specific failure and repeat-opening cases, run under Xvfb in the Ubuntu
CI lane. Main-document policy checks occur before response commit; denied requests
may already have been sent. Native Wayland, WSLg and alternating CEF/GTK windows need
manual desktop qualification. Every WebKitGTK test explicitly appends private XDG
paths without replacing existing environment entries. Adapter-specific tests use
`WebKitGTKAdapter-*`; toolkit-free policy/error tests also run with the option off.
Concurrent processes using the same executable share their persistent profile;
development packages are needed only for configure-time detection. Profiles are
restricted to `0700`; native-close tests run within the active GTK loop, and
pre-run failures remain observable. Native callbacks and policy handling use
separate named functions.
See [WebKitGTK notes](WEBKITGTK.md).

## Input decoder hardening (#267, 2026-10-18)

- Catch2 and browser test targets enable the standard library's vendor hardening mode
  (`_GLIBCXX_ASSERTIONS`, libc++ extensive hardening, or `_MSVC_STL_HARDENING=1`).
  Supporting GCC/Clang compilers also initialize automatic variables with a pattern;
  older compilers warn and retain the other hardening settings. These settings
  are private to tests/fuzzers; `WebFront::WebFront` consumers inherit none of them.
- `BuildAndTest.yml` runs the full CEF-off suite in separate Linux Clang ASan+UBSan
  and TSan configurations using `cmake/Sanitizers.cmake`. The TSan lane uses libc++
  to cover retry after a throwing `std::call_once` callback; libstdc++ with TSan
  stalled on that existing regression. Each C++ scenario has a 60-second timeout.
  UBSan diagnostics fail the process. The ASan lane also runs each of three libFuzzer targets for 30 seconds.
- `ENABLE_FUZZING=ON` builds bridge-message, HTTP-request and WebSocket-frame fuzzers
  with ASan+UBSan. Seed corpora derive from existing unit examples and live in `test/fuzz/`;
  writable copies stay in the build tree and CI uploads reproducer artifacts on failure.
  HTTP and WebSocket harnesses exercise partial receives; WebSocket decoding rejects
  oversized advertised lengths before allocating their payloads.
- Hardened regressions exposed out-of-bounds access when decoding empty strings and
  displaying an unknown HTTP method, signed overflow in HTTP version parsing, and
  undefined floating-to-integer conversions. The HTTP method buffer is now per request,
  with a regression for interleaved partial receives on two connections. Unit regressions precede
  the fixes; Jasmine also checks empty strings and rejected out-of-range integer arguments.
- Verified locally on macOS ARM64: all 167 CEF-off CTest cases pass in both hardened
  Release and ASan+UBSan builds, including `AddSubdirectoryConsumer`, which also inspects
  generated transitive compile definitions/options and link options for instrumentation leaks. Linux sanitizer
  and CEF/Xvfb execution is delegated to the PR CI; it was not run on this macOS host.
- Local fuzz smoke runs on macOS ARM64 completed 92,004 message, 743,201 HTTP and
  471,150 WebSocket inputs (15-second budgets each) without a finding after fixes.
  Homebrew LLVM 21's ASan runtime deadlocked during initialization on macOS 27;
  these runs used Apple Clang 21's ASan/UBSan and Homebrew's static libFuzzer instead.
  This local workaround is not encoded in project build settings. Short smoke runs
  do not establish exhaustive decoder or concurrency coverage.

## Embedded frontend foundations (epic #253, 2026-10-18)

- Shared URL construction (#254), registry-driven CMake selection (#255) and shared lifecycle tests
  (#256, PR #264) are merged into `develop`. The shared lifecycle cases run for CEF and WebView2,
  with WebView2-specific COM/runtime/timer cases kept separately. PR #264's GitHub Actions run
  [39105576882](https://github.com/ambroise-leclerc/WebFront/actions/runs/39105576882) passed all six
  browser tests on Linux CEF ([GCC](https://github.com/ambroise-leclerc/WebFront/actions/runs/39105576882/job/117610570356)
  and [Clang](https://github.com/ambroise-leclerc/WebFront/actions/runs/39105576882/job/117610570490))
  and all fifteen on [Windows WebView2](https://github.com/ambroise-leclerc/WebFront/actions/runs/39105576882/job/117610570553).
- The [frontend contract](FRONTENDS.md#the-contract) (#257) defines initialization once on success,
  retry after initialization failure, main-thread construction/opening for embedded engines,
  shutdown and join before returning or propagating an opening failure, unavailable-engine
  diagnostics and trusted-content scope. Mock tests cover both actions with an active server loop,
  calling-thread execution, standard/non-standard errors, failure before window creation, and
  initialization across application types.
- Main-thread validation stays adapter-specific; no generic main-thread flag or dispatcher is added.
  WKWebView (#258) and WebKitGTK (#259) now implement this check. The opt-in native alias (#261)
  selects WebView2 on Windows, WKWebView on macOS and WebKitGTK on Linux; the module demo
  accepts `WEBFRONT_DEMO_FRONTEND=Native`. Each platform adapter must be enabled explicitly.
  The system-browser default is unchanged; native rendering differs between Chromium and WebKit.
- The [shared content policy](CONTENT_POLICY.md) (#260) adds server-side bridge Origin checks,
  per-application navigation allow-lists and popup handling in CEF/WebView2, and configurable
  DevTools/context menus. External popup forwarding requires the engine's user-initiation flag;
  rejected bridge origins include a diagnostic body and warning log for alternate-host configuration.
  Local CEF-off validation passes all 192 CTest cases; browser coverage
  includes foreign/opaque-origin probes and shared navigation/popup lifecycle regressions.
- Earlier #257 validation on macOS ARM64 with Clang 21.1.8 passed the CEF-off Release build and its then-current 161 CTest cases,
  including `NewcomerDocsMatchExamples` and `AddSubdirectoryConsumer`. Tests using local sockets
  required execution outside the sandbox. CEF/Xvfb and WebView2 browser verification above is from
  the merged PR's Linux/Windows CI; these engines were not rerun locally on macOS for #257.

## Newcomer-first workflow (epic #205)

- `webfront::WebFront app; app.show();` is a complete program (`examples/hello-world/HelloWorld.cpp`). With no document root WebFront serves an embedded "Hello, WebFront!" page whose status badge shows **connected** only after `webFront.ready` resolves (WebSocket handshake acknowledgement) and an error state if the initial connection fails. `ready` does not track later disconnects.
- The Life walkthrough (`examples/life/LifeDeveloper.cpp`, `examples/life/ui/`) registers the deduced `life.step` callback and awaits its Promise from React. The standalone Greeter was removed in 0.4.
- The option-free `cmake ..` / `cmake --build .` build downloads neither CEF nor Catch2 (`ENABLE_TESTING` and `WEBFRONT_EMBED_CEF` default to `OFF`) and builds the examples into `<build>/examples`. As a subproject only `WebFront::WebFront` is added: no examples, tests, warning flags, or global compiler settings leak into the parent.
- The CTest cases `NewcomerDocsMatchExamples` and `AddSubdirectoryConsumer` guard the documented commands, snippets and subproject integration.
- Verified 2026-10-13 on Linux (Ubuntu 24.04, GCC 13.3, CMake 4.4.4): option-free build; CEF-off testing build with all CTest cases passing; Hello and Greeter pages served and driven in a real Chromium (connected badge, empty-input message, `Hello, Ada!`). **Not verified in that environment:** the CEF-enabled build and the Jasmine/Xvfb integration (the CEF download was blocked), Windows, macOS and Clang.
- Limitation: a configured document root serves arbitrary local files and is a development convenience, not a production sandbox or packaging format.

## UI components: the terminal and the tool manager

- Removed (#313, with #308): `webfront::Terminal`, `webfront::ToolManager`, `fs::TerminalFS`, `fs::ToolManagerFS`,
  `fs::ToolStore`, `fs::PackageFS`, their embedded modules and xterm.js headers, `third_party/xterm` and
  `cmake/UpdateXterm.cmake`, with their Catch2 and Jasmine tests and the fake tool registry. The environment's
  terminal and `webfront::Commands` replace them ([UI_COMPONENTS.md](UI_COMPONENTS.md) has the migration);
  `fs::safeRelativePath` moved to `SafePath.hpp`, the tarball helpers of `wf.mjs` to `js/developer/environment/tarball.mjs`.

## Intended product

WebFront is a C++23 header-only experiment for browser-based C++ user interfaces. A C++ process serves local HTML/JavaScript and exchanges typed function-call messages with the page over WebSocket. It supports the system browser, embedded CEF, optional Windows WebView2 and macOS WKWebView frontends.

The minimum demonstration milestone is deliberately narrower than the long-term vision: serve a native ES-module graph and automatically prove one JavaScript-to-C++ call and one C++-to-served-JavaScript call with Jasmine.

## Verified baseline before this milestone

- The current branch had no commits beyond `develop`; its issue-132 work existed only as local modified/untracked files.
- A CEF-off `RelWithDebInfo` build completed successfully on Linux.
- All 42 CTest-discovered Catch2 scenarios passed in 0.19 seconds.
- The latest `develop` GitHub Actions run (2026-05-31) passed its Windows MSVC, Ubuntu GCC, Ubuntu Clang, and macOS AppleClang matrix.
- The existing `webtest` executable was not registered with CTest. It opened a window, required manual closure, swallowed runtime errors, and could return success without evaluating Jasmine results.
- Existing Jasmine specs expected JavaScript calls to receive a C++ return value. The runtime bridge is fire-and-forget: `BasicWF::cppFunction` ignored `R`, and embedded `WebFront.js` did not handle `functionReturn` messages. Those assertions therefore did not describe implemented behavior.
- Registered C++ callables were captured through a forwarding-reference parameter, leaving stored callbacks with a dangling reference.
- `.mjs` already mapped to `application/javascript`; issue #132 still lacked an end-to-end module/browser demonstration.
- Documentation and CI used `ENABLE_COVERAGE`, while the test target checked `ENABLE_TEST_COVERAGE`, so requested coverage instrumentation was not enabled.

## Implemented baseline

- The default example is the native ES-module demo. Its module graph uses relative imports, calls C++, and exposes a served JavaScript function that C++ calls after module readiness.
- Registered C++ callbacks own their callable safely. Calls are asynchronous: untyped calls return no value, typed calls return a future or Promise (see below).
- Catch2 covers `.mjs` MIME handling.
- Jasmine native-module specs report their final status to C++. The executable verifies both bridge directions, asks JavaScript to close the CEF window, and returns nonzero on any failed condition.
- The browser integration is a timeout-bounded CTest test for CEF-enabled Linux builds and runs under Xvfb in a focused CI job, built with GCC and with Clang. CEF's `NO_UNIQUE_ADDRESS`, `ALWAYS_INLINE` and `NOINLINE` macros break stdexec headers included after CEF's headers, which only Clang 19+ (and, by stdexec's configuration, MSVC) reveals (#243): `frontend/CEF.hpp` includes `networking/SenderHeaders.hpp`, which lists every stdnet and stdexec header WebFront uses, first, and the Catch2 case `CEFMacrosTests.cpp` compiles `WebFront.hpp` after those macros. Verified 2026-10-16 on Linux with Clang 22: the CEF build of `webtest` failed before the fix and `WebFrontBrowserIntegration` passes after it. Not verified: CEF with MSVC or Apple clang.
- `WEBFRONT_EMBED_CEF=ON` uses CEF 154.0.34 (Chromium 154.0.8037.98, package `ambroise-leclerc/CEF@154.0.34`). An existing CEF 137 build directory must be reconfigured from scratch. On Windows, `libEGL.dll`, `libGLESv2.dll`, `dxcompiler.dll`, `dxil.dll` and `vk_swiftshader_icd.json` are copied only when the distribution ships them (CEF 154 has no ANGLE DLLs, #249). Verified 2026-10-16 on Linux (GCC 16.1, Unix Makefiles): the CEF build with all 138 CTest cases passing, `WebFrontBrowserIntegration` included, and the CEF-off build with all 137; with Clang 22, the CEF build of `webtest` and `WebFrontBrowserIntegration`. **Not verified:** CEF 154 on Windows and macOS (the macOS ARM64 framework requires macOS 13.0+). The earlier multi-example build intermittently raced while copying CEF `locales/` to its shared output directory. The retained examples now use `<build>/examples`.
- The bridge carries all JavaScript numeric typed-array variants in both directions. C++ `std::vector`, `std::array`, and `std::span` values use the matching typed-array encoding, while decoded parameters own their storage.
- `js/core/WebFront.js` is the readable browser bridge source. CMake deterministically regenerates the embedded `include/webfront/system/WebFrontJsData.hpp` asset and CTest rejects a stale generated copy.
- Encoded HTTP assets (#274): accepted gzip is served unchanged; otherwise the server decodes
  gzip to identity with the decoded length, no `Content-Encoding` and `Vary: Accept-Encoding`.
  The default decoded-byte budget is 4 MiB and applications can override it through their buffer
  policy. Other codings remain pass-through only. Fallback runs synchronously per request,
  including HEAD; caching and byte-wise decoding remain follow-ups. See [HTTP asset encoding](HTTP_ASSETS.md)
  for negotiation rules, errors, strict stream validation and budget configuration.
  Regression tests and gzip/HTTP fuzzers cover decoding and encoding preferences.
  Verified 2026-10-19 on Linux for #274: the CEF-off build/CTest suite, CEF/Xvfb integration and
  WebKitGTK integration passed. The macOS results above describe earlier platform validation.
- First-party bridge/component text assets are embedded **verbatim rather than compressed**, so `HTTPServer` serves them without a `Content-Encoding` header. Deflate streams are not reproducible across the zlib builds on our macOS, Linux, and Windows runners, so a pre-compressed blob could not be verified by the staleness check. Restoring compression — either a reproducible build-time gzip or on-the-fly compression keyed on `Accept-Encoding` — is tracked as a follow-up. The fallback HTML and module have readable sources under `js/core/fallback/`; `favicon.ico` uses gzip. Legacy third-party Jasmine/React/Babel assets now use gzip for WKWebView local-HTTP compatibility (#258); their decoded payloads are unchanged. Regenerate the text headers with `cmake --build build --target webfront-embedded-assets`. Sources and generated headers use LF on every platform so the CTest freshness checks compare identical bytes.
- The fallback page awaits `webFront.ready` to display the initial bridge connection result. This promise resolves after the handshake acknowledgement and rejects if the initial connection fails or closes; it does not track later disconnects. Jasmine exercises failure against a test-owned HTTP server on an OS-assigned loopback port that rejects WebSocket upgrades, including the case where the application never consumes `ready`.
- WebSocket writes own their payload buffers until asynchronous completion and are serialized per connection.
- The HTTP server looks files up by the request path without its query string (`/app.js?v=2` serves `app.js`); the path is cut at the first `?` before percent-decoding, so an encoded `%3F` stays part of the file name, and the `..` check applies to the decoded path only. The query is not passed to filesystems.
- WebSocket reads dispatch coalesced frames in order and retain incomplete frames between reads. Messages a browser sends in several frames are reassembled. A message larger than the buffers policy's `maxMessageSize` (64 MiB by default), in one frame or adding up its fragments, closes the connection as soon as a frame header announces it, before its payload is buffered. Control frames have their own 125-byte limit (RFC 6455) instead; a larger or fragmented one closes the connection, as does a misplaced continuation. Header completeness follows the encoded length and mask, including the 8-byte masked header for 16-bit payload lengths.
- Strings cross the bridge with a 16-bit length, so both encoders reject strings over 65535 bytes instead of corrupting the message (#231). In C++ a call with such an argument throws `std::length_error` before anything is sent, and a callback returning one rejects the JavaScript Promise with `WebFront strings cannot exceed 65535 bytes`; an exception message is cut to the limit on a UTF-8 boundary. In JavaScript the call or return is rejected with a `RangeError`. Covered by Catch2 encoding and responder tests and by browser specs.
- Several handlers can observe connected browsers: `subscribeUIStarted` adds one for as long as the returned `UISubscription` lives, after the application's `onUIStarted` handler, and a callback whose first parameter is the application's `UI` receives the calling browser (#233). Covered by Catch2 subscription and caller tests and by a browser spec in which C++ calls back the calling page; the browser test also requires a subscriber to be notified.
- Correlated result messages now support asynchronous C++ futures and JavaScript promises, including void completion, missing-function errors, callback exceptions, malformed-return rejection, and disconnect rejection. Untyped calls remain fire-and-forget.
- `AGENTS.md` is the canonical development guide for supported coding agents.
- The default frontend always uses the system browser, including CEF-enabled builds. Applications opt into embedded windows through
  `WebFrontWithFrontend<frontend::CEFFrontend>`; selecting CEF without compiled support throws during construction. On macOS,
  executable discovery and framework paths follow the running target and CMake's `../Frameworks` deployment layout.
- Windows has an optional `WebView2Frontend` (#137), enabled with `WEBFRONT_EMBED_WEBVIEW2=ON` and selected explicitly.
  Its pinned NuGet SDK and static loader are fetched only when enabled; the Evergreen runtime remains a prerequisite.
  The adapter owns STA COM, the window/message loop, resizing, navigation and controller shutdown, with writable per-executable
  profiles in LocalAppData. `WebFrontApp` selects it with `WEBFRONT_DEMO_FRONTEND=WebView2`.
  Windows builds require an MSVC-compatible compiler; MinGW is rejected before SDK setup. Built-in frontends share URI-reference encoding.
  Native focus and move messages forward to `MoveFocus(PROGRAMMATIC)` and `NotifyParentWindowPositionChanged`;
  controller mocks cover these messages, reentrant shutdown and the startup background. HRESULT diagnostics use eight-digit hexadecimal values.
  SDK loader selection follows the compiler's x86/x64/ARM64 target, including x64 builds on ARM64 hosts;
  a portable CMake regression simulates cross-target builds without downloading the SDK.
  Only initial navigation failure is fatal; later navigation errors and transient renderer unresponsiveness keep the window open.
  Closing or failing during startup cancels its timer and preserves the first error. The loop returns when the window is destroyed,
  even if a runtime never completes creation; weak references protect late callbacks. Renderer exits remain fatal rather than reloading the page.
  Profile identity uses a case-sensitive FNV-64 executable-path hash. The adapter supplies no DPI-awareness manifest;
  applications must provide one to avoid blurry Windows scaling on HiDPI displays.
  The [shared content policy](CONTENT_POLICY.md) restricts embedded navigation and popups and authorizes bridge origins server-side;
  DevTools and context menus remain configurable and enabled by default. Windows/Shell/WRL header names leak into consumers;
  qualify `std::byte`. Implementation types live under `frontend::detail::webview2`. The runtime-creation timeout defaults to 30 seconds,
  is configurable through `WebView2Options`, and stops before page navigation. Profile directories are also configurable.
  All built-in frontends receive the configured address and preserve URI query/fragment syntax and existing percent escapes.
  Controller closure is idempotent and reports HRESULT failures; DPI-change messages apply the suggested window bounds.
  The Windows CI matrix runs the shared Jasmine bridge suite and timed lifecycle tests under WebView2; CEF/Xvfb remains the Linux lane.
  The WebView2 browser label runs separately with `--no-tests=error`. CI's Evergreen bootstrapper URL is mutable;
  a downloaded installer must carry a valid Microsoft signature, and installed executable versions are logged.
- Embedded frontend lifecycle coverage is shared through `test/web/FrontendLifecycle.cpp` (#256),
  selected with `WEBFRONT_TEST_FRONTEND` alongside Jasmine. Each registered test platform inherits
  seven timeout-bounded `web-integration` cases: no bridge, immediate page closure, rejected ready,
  failed initial navigation, encoded paths, blocked navigation/popups and an explicit navigation
  allow-list. CEF/Linux uses Xvfb; WebView2 retains its separate
  COM, runtime, native-close, repeated-show and timer regressions. Building `webtest` also builds
  the shared lifecycle runner. CEF now closes a failed initial main-frame navigation and throws
  after shutdown; cancelled requests and failures after the initial page has loaded do not trigger
  this startup error.
  Verified 2026-10-18 on Linux (GCC 16.1, CMake 4.2.3, CEF 154): all 156 CEF-off CTest cases and
  all six CEF/Xvfb browser cases pass. The failed-navigation case timed out before the CEF fix and
  passes afterward. Configure-only tests cover the shared suite under WebView2, absent Xvfb and
  a newly registered engine; native WebView2 execution was not repeated in this Linux environment.
- Frontend CMake options, platform constraints, selector choices/defaults and browser CTest wiring share `cmake/Frontends.cmake`. Adding an engine needs one registration plus its support module. CEF browser tests remain restricted to Linux under Xvfb until Windows/macOS runner deployment is validated; enabled WebView2 retains its Windows Jasmine and lifecycle cases. With no embedded engine enabled the CEF diagnostic runner still builds without registering a browser test.
  Verified 2026-10-17 on Linux (GCC 16.1): the complete CEF-off CTest suite and the CEF 154/Xvfb browser integration pass. The optional `FrontendSelectionMultiConfig` case is registered when Ninja is available. Configure-only fixtures cover platform defaults, unsupported platforms, Xvfb wiring and a new registered engine. Native CEF execution on Windows/macOS remains unvalidated: those CI lanes keep CEF disabled; Windows runs the WebView2 browser suite. CEF setup leaves dependency sources untouched, excludes upstream demo targets from the default build and requires a real static wrapper at configure time; the Apple configuration path is covered by an offline fixture, not native execution.
- Local verification for #137 (2026-10-17, Windows, MSVC 19.44, x64): the CEF-off build passes all 153 CTest cases;
  the WebView2-enabled build also passes all 158 non-browser CTest cases. All examples, the adapter and the Jasmine runner compile.
  All 12 browser CTest cases pass with an intact official Microsoft runtime 155.0.4283.62: shared Jasmine bridge checks,
  repeated no-bridge closure, early native closure, rejected `ready`, initial and later navigation failures, incompatible COM,
  missing runtime, queued startup timers and missing creation callbacks after closure or failure, a held page resource after runtime creation,
  and a page whose filename contains a space and `#` with a separate query and fragment.
  The new timer, navigation, process-policy, path-encoding and compiler-guard regressions failed before their fixes.
  The loader-architecture regression also failed before selecting by compiler target. The native socket shutdown test
  now waits for the bridge handshake, so it cannot stop between the HTTP 101 response and WebLink creation.
  Startup closure tests hold an actual environment-creation completion handler without invoking it, then invoke it after
  session destruction. No production pending-creation counter is kept. Focus, move, background and HRESULT regressions failed before their fixes.
  Early native closure is posted from a same-thread window-creation hook, without polling window titles.
  Architecture tests exercise explicit compiler IDs, generator-platform fallback and target-macro fallback for clang-cl,
  including rejection of unknown targets and ARM64EC. A real TCP test confirms that construction listens before run() starts.
  The native-module demo loads its UI and its JavaScript-to-C++ callback was exercised from the native window.
  Signal Studio (`WebFrontApp react.html`) uses the embedded React, ReactDOM and Babel assets to display a synthetic waveform
  generated by a C++ callback. The README screenshot includes its real WebView2 native window frame.
  Two C++ scenarios check the sample data and parameter validation; the shared Jasmine suite drives the actual React page,
  checks local asset loading, changes a parameter, and verifies recovery after a rejected C++ call.
  The system's Evergreen 155.0.4283.45 installation remains broken: reading its compressed `ffmpeg.dll` fails with
  `ERROR_BAD_COMPRESSION_BUFFER` (605, HRESULT `0x8007025D`), even when only computing its SHA-256. This DLL is hard-linked
  between EdgeCore, Edge and EdgeWebView. An independent Win32 probe reproduced the renderer launch failure (reason 4, exit code 71).
  For local validation, the official runtime CAB was extracted under an ignored build directory and selected with
  `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER`; the runtime executable and `ffmpeg.dll` both have valid Microsoft signatures.
  No system runtime files or security settings were changed. Fixed-version runtime packaging remains out of scope.

- Follow-up verification for #254 (2026-10-17, Ubuntu 26.10 under WSL2, GCC 15.3, CEF 154.0.34):
  `WebFrontBrowserIntegration` passes under Xvfb. The shared Jasmine suite loads `encoded path %23.html`
  and verifies the decoded filename, query and fragment. This completes the CEF check alongside the
  Windows CEF-off baseline (153 CTest cases) and the WebView2 Jasmine and path-encoding checks.
  The URL builder also escapes a literal path `+` as `%2B`, with a failing-before Catch2 regression
  and a browser check that fetches `a+b.html` using a C++-constructed URL.

Local verification on 2026-07-19 completed successfully: the CEF-off build passed all 47 CTest cases, including deterministic asset and typed-array protocol coverage, and a CEF-enabled build passed `WebFrontBrowserIntegration` under Xvfb. The browser test exercised every supported numeric typed array from C++ to JavaScript and JavaScript to C++, ordinary tuple arrays, final Jasmine reporting, and automatic CEF shutdown.

## Known limitations and next decisions

- Result values are currently limited to the bridge's existing scalar, string, tuple, numeric typed-array and packed bitset parameter types; richer object/DOM values remain out of scope.
- `webfront::Task`, `app.spawn(task)`/`ui.spawn(task)` and per-task `TaskHandle::requestStop()` provide application-owned coroutine lifetime without exposing an execution context. The compiled `WebFrontAwaitBrowser` example matches the README; `app.shutdown()` stops and joins before returning; spawn is callable from any thread while the application lives and returns a stopped handle if shutdown rejects it. Destruction keeps owners alive and logs/waits if an external operation delays the join, whereas explicit shutdown reports absent context progress.
- `ui.jsFunctionSender<R>(name)(...)` (default `R = void`) adds lazy, awaitable typed calls, with value/error/stopped completion and local receiver-token cancellation. Futures and JavaScript Promises remain supported without cancellation. JavaScript already dispatched continues; late replies are discarded. See [execution design](ASYNC_EXECUTION.md).
- Terminal commands (`webfront::Commands`) stream output with fire-and-forget calls: `context.write` has no flow control, `co_await context.print` waits, without polling, while the browser has more than 32 frames to read.
- Repeated cancellation of JavaScript functions that never respond reserves their 16-bit IDs until disconnection. Enough such calls exhaust IDs and subsequent calls fail with `Too many outstanding WebFront calls`. Calls have no automatic timeout or per-write delivery acknowledgement.
- Server-owned counting scopes track HTTP, WebSocket and application operations through destruction. Shutdown drains until scope join completes, independently of unrelated context work. Regression tests cover pending reads, connection lifetime, sender errors, cancellation and result/stop races; the shared Jasmine suite exercises sender value/void/error and late-result cancellation.
- Ordinary JavaScript arrays encode as tuple-like parameters; only numeric typed arrays use the homogeneous array wire representation.
- `NativeDebugFS` exposes development files and is not a production packaging format.
- CEF is a large optional dependency. Only the focused Linux integration job should download it for the minimal baseline.
- React/Babel assets remain examples rather than the core demonstration. Modern Node/Vite integration, overlays, and packaged filesystems require separate product decisions. WebView2 fixed-version runtime packaging remains out of scope.

## Local CI over SSH (#346)

The eleven-lane description and workflow drift checks now have a Python standard-library orchestrator, commit bundles, verified
offline dependency/image kits, per-lane Linux containers without network access, persistent CPM/ccache directories, log and
artifact collection, bounded execution and explicit incomplete/failure statuses. The offline protocol regression uses SSH/Docker
stand-ins. Native Windows execution now launches a limited interactive scheduled task, initializes MSVC, checks WebView2 and
the owner-prepared public egress firewall policy, caps compilation at four jobs, owns descendants through a Windows Job Object,
and collects diagnostics before cleanup. Offline Windows regressions include real PowerShell binary transport and Job cleanup.
Native macOS execution now uses a temporary Aqua LaunchAgent, reports selected Xcode/AppleClang and OS versions, runs the
WKWebView lane with a verified Mac kit and inherited process network restriction, and reports missing sessions as skipped.
System WebKit XPC services are outside that process restriction. Real Linux/Windows/macOS host qualification, XPC egress
isolation, the complete eleven-lane run and comparison with GitHub timings remain open;
this is not yet a qualified replacement for running any of the GitHub jobs. See [LOCAL_CI](LOCAL_CI.md).

The macOS investigation for #368 (2026-10-25, macOS 27.0.1 ARM64) passes 27 worker regressions, including an actual temporary
Aqua LaunchAgent, its inherited socket restriction, descendant termination and repeated stop. A supplementary Homebrew
Clang 21.1.8/CMake 4.3.1 CEF-off Release build passes all 265 CTest cases after fixing explicit SDK propagation to
`AddSubdirectoryConsumer`; the supplementary WKWebView Release build passes all 340 cases, including the bridge, lifecycle,
benchmark and developer tests (195.54 seconds). Those browser tests run in the principal account outside the worker's network
policy. This is not the pinned AppleClang/Xcode lane: the development host has only selected Command Line
Tools, CMake differs from the lane's 4.4.4, SSH to `Mac` is refused, and no dedicated standard CI account is provisioned.
The exact-lane run, cold/warm timings, SSH recovery and WebKit XPC egress qualification remain pending.

The Windows investigation for #368 (2026-10-25, Windows 11 build 22631, MSVC 19.44.35229) passes all 342 CTest cases in a
CEF-off/WebView2 Release build, including 73 browser integration cases, and 29 native-worker regressions. A real preflight
exposed localized PowerShell output corruption and CLIXML error logs; an explicit UTF-8/text diagnostic fix has a failing-then-
passing native regression. A second CMake 4.4.4/sccache 0.18.0 Release build using the packed, extracted and verified Windows
kit also passes all 342 CTest cases (341.33 seconds), with FetchContent disconnected and zero JUnit failures/skips.
Cleaning and rebuilding the same tree reuses 155/155 sccache entries and succeeds in 22.5 seconds; this is local cache reuse,
not a second SSH lane run. The development account has a non-elevated administrator token, not the required standard-account
boundary; no dedicated CI account or SSH server is
provisioned, and the required egress rule is absent. The real session worker correctly rejects that host before configure.
Dedicated-account SSH/task/network qualification, cold/warm timings and disconnect recovery remain pending. See the
[Windows qualification record and setup commands](LOCAL_CI_WINDOWS_QUALIFICATION.md).
