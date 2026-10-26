# Changelog

## Unreleased

- ADR-018 (Proposed, #378) studies DOM access from C++: a batched, validated mutation stream into page-lent islands, React and
  other UI libraries kept as owners of their DOM (external store, islands), optional C++26 reflection for generated
  interfaces. Throwaway prototypes and their measurements live in `test/web/dom/` (manual target `webkitgtk_dom_prototype`,
  never built by CTest or CI). They also found a 40 ms delay on callbacks that call `ui.jsFunction` before returning
  (`TCP_NODELAY` is not set); the fix is tracked by #379.

## 0.5.0 - 2026-10-26

- Gzip identity fallback reuses a bounded validated-representation cache for GET and HEAD (#281).
  Cold decoding uses buffered bit reads and a small Huffman lookup (#282), with unchanged
  allocation behavior, strict stream validation and output budgets. Release benchmarks and
  test-only zlib differential/sanitizer validation are recorded in `docs/GZIP_DECODER_MEASUREMENTS.md`.

- `WebFrontLifeDeveloper` starts without argument on `./life-project` (created when missing, reused untouched otherwise) and the
  pinned environment; the explicit `<project> [<environment-url> <sha512-integrity>]` form remains. At configuration, the
  native window engine of the platform is enabled automatically when its dependencies are found (top-level builds with examples,
  without `ENABLE_TESTING`; `-DWEBFRONT_AUTODETECT_NATIVE=OFF` disables it, an explicit `WEBFRONT_EMBED_*` value always wins).
  A missing engine does not fail the configuration: CMake explains how to install it and the Life examples are built for the default browser instead. The README Game of Life walkthrough builds with `mkdir build`, `cd build`,
  `cmake ..` and `cmake --build .` (#366). The developer terminal drops its redundant banner line; the sources advance to environment 0.6.3 (unpublished, the pin stays on 0.6.2).
  Automatic engine values stay outside the cache so an explicit choice equal to the previous detection is still respected;
  removing an explicit engine cache entry resumes detection. Platforms without a registered engine configure the browser fallback.

- Cold gzip identity fallback uses one server-owned worker with bounded jobs, consumers and
  buffer capacities (#283). The new compressed-input cap defaults to 4 MiB and returns 500
  before cache lookup when exceeded, including HEAD; configure `maxGzipEncodedAssetSize`
  for larger assets. Queue/memory saturation and preparation allocation failures return 503.
  Cancelled work skipped before decoding and decoder exceptions cannot poison later consumers.
- Transport benchmarks are manual targets, excluded from CI, CTest and default builds (#283).
  Short direct-channel correctness contracts remain in CTest for WebKitGTK, WebView2 and WKWebView.

- Remove the local SSH CI orchestrator, container definitions, native session workers and matrix drift checks.
  The GitHub-hosted workflows remain the CI execution path.

- The allocation-counting regression is omitted from ThreadSanitizer builds, whose C++ runtime
  defines the same global `new`/`delete` operators; ordinary and ASan/UBSan builds retain it. macOS adds
  `WKWebViewTransportBenchmark` (HTML bridge, native strings, `callAsyncJavaScript` and base64 bytes) and separate native
  API contract tests. ADR-017 records the measurements and the public Cocoa API constraints on a future direct transport.

- Linux transport performance now has a dedicated Release CI lane without coverage, repeated three times with retained
  JSON, environment and CTest reports; the Debug/coverage desktop lane stays in place. ADR-017 distinguishes mandatory HTML
  availability from the selected default transport and qualifies mobile network rules by OS version.

- ADR-017 (Proposed, #363) prepares direct access to embedded frontends (native resource schemes and message channels) for
  future Android, iOS and other mobile frontends, while HTTP/WebSocket stays the normative path of every frontend and of future
  microcontroller targets. The binary format of ADR-003 is carried unchanged; no API or wire change is made. The new
  `WebKitGTKTransportBenchmark` CTest case (labels `web-integration` and `performance`) compares the bridge with WebKitGTK's
  message handlers and `evaluate_javascript` in one window; it fails on a wrong echo, never on a timing, and writes a JSON report.
  `WebView2TransportBenchmark` does the same on Windows with `WebMessageReceived`, `ExecuteScript` and a shared buffer
  (`ICoreWebView2SharedBuffer`): WebView2 string messages are slower than the HTML bridge from 32 KiB and for C++ to JS, the
  shared buffer is 2 to 6 times faster. The figures leave out the codec and the thread hop a real transport adds, so they
  overstate the gain of a direct path. The benchmark exposed two per-message costs of the bridge, fixed with regression tests:
  `Messages.hpp` wrote to the standard output for every encoded parameter, and `Frame::addBuffer` formatted a hex dump of
  every buffer before the log level was tested.

- The README animation draws the C++ application inside the image, beside the WebFront window (#359): it is no longer cropped by the table column on narrow screens and in the GitHub app, and weighs 0.4 MB instead of 1.2 MB. The code stays in the README as text, in a collapsed block; `docs/tools/render-life-bootstrap.py` reads it from there.

- The manifest and the lock of the repository move to `js/developer/` (#354): the driver and the CI select them with `--manifest-dir js/developer`, and the closed list of the root loses two entries (9 left).

- The test bootstrap environment is the published 0.6.2 (#354), which installs the archives of `@types` packages and honours a manifest directory; the README walkthrough no longer needs a local archive for `wf check`.

- The manifest and the lock of a project may live in a directory of the project (#354): `DeveloperBootstrapOptions::manifestDirectory` (validated, passed in the bootstrap configuration only when set) and `webfront-wf --manifest-dir`; the environment derives `manifestPath` and `lockPath` from it, and a script still never writes them. Paths inside the manifest stay relative to the project root. Environment 0.6.2, not yet published; an earlier environment ignores the option.

- Simpler repository root (#352): `.devcontainer/` is removed (nothing used it, and its Dockerfile duplicated the toolchain of the CI by hand); `.gitattributes` drops its generic Visual Studio and document rules (177 to 69 lines, the attributes of every tracked file are unchanged); `.clang-tidy` becomes `cmake/clang-tidy.yaml`, passed to clang-tidy by `cmake/StaticAnalyzers.cmake`, and `.clang-format` moves next to the C++ it formats, with a copy in `include/webfront/`, `test/` and `examples/` so that their styles may diverge; the layout guard records why every other root entry stays.

- Documentation of the standalone developer environment (#317, epic #303): the README walkthrough uses `wf create`, `init`, `install`, `dev`, `check`, `test` and `embed` on the pinned environment; `DEVELOPER_ENVIRONMENT.md` opens with the commands, the direct statement of its limits (no sandbox, browser packages only, installation scripts not run) and the maintenance of TypeScript through `wf check`; the 0.4.0 "prepared, not published" text is removed; PROJECT_STATUS starts with the current state; ADR-016 is 1.5.

- Remove the retired PI starter from source distributions and `wf create`; Life remains the maintained React/C++ starter.
  Preserve real-time fallbacks in scoped test clocks, release preparation cancellation before the test frame runs,
  and retain independent reproducibility, bootstrap-refresh and packaged permission checks. The attribution audit
  ignores user signature/log display settings.

### Fixed

- The bridge no longer pays for diagnostics on every message (#363): the encoder of `Messages.hpp` wrote to `std::cout` for
  each parameter, and `Frame::addBuffer` formatted a hex dump of each sent buffer before `log::debug()` tested the level (21.7 MB
  allocated for a 1 MiB buffer). A 32 KiB echo over the HTML bridge drops from about 2.4 ms to 0.7 ms on WebKitGTK. Regressions:
  `Encoding bridge messages writes nothing to the standard output` and `FrameBufferNotFormattedWhenDebugDisabled`.
- The developer store reads and writes project paths beyond MAX_PATH on Windows (#349): `projectPath` takes the extended-length form from 200 characters, as does a project root longer than MAX_PATH (`canonicalDirectory`), containment is checked on the ordinary spelling, and staged files get the same treatment. The MSVC option is now `/analyze:stacksize65536`, one argument: with a space, sccache read `65536` as a second source file and refused to cache 48 compilations of the Windows CI build (about 9 minutes against 25 seconds on Linux).
- The installer of the developer environment reads npm archives rooted at any single directory, as npm does (#316): the archives of
  `@types/*` are rooted at the package name and were refused as "Invalid npm tarball root". A mixed or escaping root is still refused.
- The Life starter type-checks under TypeScript `strict` (`useState<Uint8Array>`), which `wf check` now proves.
- Script injection works before the page body exists, including a driver handshake completed during HTML parsing.

- `wf test` supports WebKit engines without `AbortSignal.any()` (#335), preserving cancellation/deadline reports and
  exit codes. Deadline and cleanup timers and the external cancellation listener are released on every exit.
  The driver also remembers a watchdog close requested before the browser handshake; a late connection closes
  without launching a command. Environment sources advance to 0.6.2 while the release and test-bootstrap pins remain on published 0.6.1.
- Browser regressions advance deadlines, grace periods and dev polling explicitly instead of sleeping. Native command
  cancellation uses a release handshake, repeated type checks reuse immutable fixture bytes, cache/command fixtures
  drop the large application-only dependency graph after the Life checks, and the shared bridge
  runner avoids full binary debug dumps. Retired PI and Signal Studio application tests are removed; Life retains
  React/TSX, preview, reproducible generation and rootless embedded coverage. Reproducibility compares two identical
  builds, followed by a separate bootstrap refresh; packaged-script permission checks remain end-to-end.


### Added

- Developer journey qualification (#316, epic #303): `Journey*` CTest cases run `wf create`, `init`, `install`, `check`, `test`, `build`
  and `embed` on one project in an empty directory from the local registry, compile the generated header into the Life application
  and run it from an empty directory, then check a script under denied permissions and an emptied cache offline and restored. The test
  registry gains `@types/react`, `@types/react-dom` and `csstype`. ADR-013, ADR-014 and ADR-015 are accepted.
- `third_party/` is removed (#319, epic #303): `fflate` 0.8.3, the last vendored source, is a locked package of the repository's
  `wf` project, imported by name from `js/developer/environment/embed.mjs`; its notice comes from the installed package.
  The environment archive stays byte for byte identical. The test registry downloads it at configure time, and
  `RepositoryLayout` now rejects `third_party`, `vendor` and `node_modules` paths as well as any unlisted root entry.
- Tests, regeneration and CI without a JavaScript runtime or package manager (#318, epic #303). Configuring with `ENABLE_TESTING=ON` downloads, and
  verifies against their SHA-512, the pinned bootstrap environment and the real packages of the tests' npm registry
  (`test/web/registry-packages.json`; `-DWEBFRONT_REGISTRY_DIR` for a machine without network access); `wf run test-registry`
  builds the registry, the environment is built from the current sources and the fixtures are assembled by CTest itself, so
  `WEBFRONT_DEVELOPER_FIXTURES` and `WEBFRONT_REQUIRE_NODE` are gone. The 86 JavaScript tests became 84 Jasmine specs
  (`test/developer/specs/`) run by the repository's own `wf test`, plus CMake tests for the bundle checksums and the reserved
  command names. The headers of the core are rendered and checked by `js/core/EmbedAsset.cmake` in pure CMake (ADR-016
  v1.3), with a Catch2 scenario that decodes each of them. `NoNodeDependency` audits every file and
  `NoNodeDependencyRejectsInjections` proves the guard. The Node.js tools, `vendor/package*.json` and the comparison job
  of *Developer packages* are deleted; every CI job removes the JavaScript runtimes from the runner first. The record of
  each former tool and test is `docs/NODE_TOOLS_MIGRATION.md`.

- The developer environment builds itself (#315, distribution 0.6.1, published as `developer-v0.6.1` and pinned by `DeveloperReleaseData.hpp`, and by the bootstrap of the tests): the
  repository is a `wf` project (`webfront.project.json`, `webfront-packages.lock.json`) and the scripts of
  `js/developer/scripts/` run in the pinned bootstrap environment with `webfront-wf`. `build.mjs` bundles the environment
  with the locked esbuild-wasm and writes `environment.tgz`, the starters, `provision.json` and the pinned header (with
  `--refresh-bootstrap`, the bootstrap bundles, their checksums and the core headers); `site.mjs` stages the Pages site
  and refuses to change a published version; `pin.mjs` checks the pinned header against the published archive; `embed-core.mjs` regenerates the core headers. The bytes
  are those of the Node.js build, twice in a row (CTest `SelfBuild*`, job `wf-package` of *Developer packages*). The
  publication runs without Node.js (`pages.sh`, `archive-release.sh`, a shell tag check); `-DWEBFRONT_REQUIRE_NODE=OFF`
  configures a test build without it. The inventory of the Node.js tools and their destination is
  `docs/NODE_TOOLS_MIGRATION.md`; the maintainer procedure is `js/developer/tools/PUBLISHING.md`.

- `webfront-wf` test driver and bootstrap provisioning (#321, developer distribution 0.6.0 published as `developer-v0.6.0`, pinned by `DeveloperReleaseData.hpp` and by the bootstrap): runs one `wf` command without interaction on a project
  and exits with its status (64 usage, 70 bootstrap failure, 71 time limit, 72 no result), with an optional JSON report.
  `cmake/WebFrontBootstrap.cmake` pins the bootstrap environment (0.6.1) and, with `ENABLE_TESTING=ON`, downloads
  and verifies it at configure time, or takes it from `WEBFRONT_BOOTSTRAP_DIR`. `environment.json` declares `protocol` and
  `api`; `webfront.developer.config` exposes the supported versions and the loader refuses an incompatible archive with an
  explicit diagnostic. The Linux CEF job blocks the public network after configure.

- `wf test [--timeout <s>] [pattern]` (#310, environment sources; release pin unchanged): builds the manifest's
  `test.specs` with esbuild-wasm and runs them under the project's locked `jasmine-core`, obtained by `wf install` from
  the registry, in a hidden same-origin frame. Progress and a summary stream to the terminal; the structured report
  (counts, failures, pending specs) is bounded, and the exit code is 0, 1 (failure, load error, nothing run), 124 (time
  limit) or 130 (Ctrl+C). `runNonInteractive` runs a command without a terminal, sends `{exitCode, output, report}` to an
  application's C++ function and ends the session. `DeveloperFS` also serves `.webfront/developer/test/`.

- `wf build [entry]` / `wf dev [entry]` (#309, environment sources; release pin unchanged): build all manifest
  entries to `.webfront/developer/build/` without opening the preview, or continuously rebuild one preview
  with a persistent esbuild/WASM context. Sequential 750 ms polling watches read inputs and missing imports
  (1,024 paths, 32 MiB per pass). Build/publication errors preserve the last good preview; diagnostics with
  file/line/column stream to the terminal and overlay the preview. Corrections resume rebuilding; Ctrl+C
  terminates the Worker, stops polling and aborts staged output.

- Project manifest `webfront.project.json` (format 1: `name`, `version`, `dependencies`, `registry`, `scripts`,
  `entries`, `providers`, `test`, `permissions`), strictly validated; `wf init`, `wf install [--update]` and
  manifest-driven `wf preview`/`wf embed` in developer distribution 0.5.0, which `DeveloperReleaseData.hpp` pins
  (published as `developer-v0.5.0`). The lock records the digest of
  the manifest dependencies and reports a desynchronized state. `developer::Store::commitGroup` publishes
  manifest and lock through a synced intent journal with crash recovery and a concurrent-install precondition.
  New bridge callbacks `webfront.developer.state` and `webfront.developer.commitGroup`. Starters ship a manifest;
  `wf install` now needs `wf init` first, which adopts the roots of an existing lock so a project locked before 0.5.0
  keeps its locked graph. A committed publication that cannot be completed does not prevent opening the project:
  it is reported (`developer::IncompletePublication`) and completed by the next command.
- `wf restore [--offline]` (developer distribution 0.5.0): rebuilds the package cache from the lock alone, at the
  locked versions and integrities, idempotent and interruptible; `--offline` only checks the cache and lists the
  missing or corrupted packages. Cached packages carry a record of their files' digests. The registry is the
  application's option, else the manifest's `registry`, else registry.npmjs.org; `wf install` and `wf restore`
  print it with its origin. A reproducible local npm registry fixture (`js/developer/tools/registry-fixture.mjs`)
  serves the tests.

- `wf run [--timeout <s>] <script>` and `wf exec <package>[:<entry>]` (developer distribution 0.5.0): WebFront
  scripts (JS/TS bundled by esbuild-wasm, `default async function (context)`) run in a dedicated Worker with the
  `context` API (`args`, `signal`, `console`, `project.read/readText/list/write`, `packages.resolve`, `fetch`,
  `exit`) under the manifest `permissions`. `wf exec` runs only `webfront.bin` entries, never npm's `bin`. Node.js
  imports are refused while loading. Ctrl+C or the time limit aborts `context.signal`, then terminates the Worker
  after a 2 s grace period; staged writes are aborted, committed ones complete. Exit codes reach the terminal.
  New bridge callback `webfront.developer.list` (`developer::Store::list`).
- `wf check [--timeout <s>]` (developer distribution 0.5.0): a TypeScript type check in a dedicated Worker with the
  `typescript` of the lock (5.x and 6.x; older versions and the native TypeScript 7 are refused). It loads an
  in-memory snapshot (lib files, declarations of the locked graph, project files covered by `tsconfig.json` or the
  manifest entries; at most 10,000 files and 64 MiB), then compiles synchronously over it with TypeScript's own type
  resolution limited to the locked graph. Diagnostics print as `file(line,column): error TScode: message`; errors
  end with code 1. Nothing is written (`noEmit`). Ctrl+C and the time limit terminate a running compilation after
  the 2 s grace period. The local registry fixture serves typescript 5.9.3 and 6.0.3, typed and `@types` packages.

- The environment's terminal (developer distribution 0.5.0, #308) becomes WebFront's complete terminal: built-in
  `help`, `clear`, `history` and `exit`, streamed output, Ctrl+C for every command, per-project persistent history,
  multi-line paste run line by line, multi-row redraw, CJK and emoji widths shared with xterm.js, exit codes and
  completion of commands, `wf` subcommands, manifest scripts, entries, providers and project paths.
  `webfront::Commands` (`<webfront/Commands.hpp>`) adds an application's C++ commands to it: coroutines spawned per
  browser session through the bridge functions `webfront.commands.list`, `.start` and `.cancel`, streaming their
  output, stopped by Ctrl+C, the browser's disconnection or shutdown; the terminal does not wait for a stopped
  command; `co_await context.print(text)` waits for a slow browser. New `app.resumeAfter(duration)`/
  `ui.resumeAfter(duration)` timer sender, `app.subscribeUIClosed(handler)`, `app.connectedBrowsers()`, `UI::id()`,
  `UI::pendingWrites()`, `UI::writesAtMost(limit)` (woken by the write that drains the queue), `app.findLink(id)` and
  `json::Reader::array`. `cppFunction` returns false when the name was
  already registered, and `hasCppFunction(name)` tells whether it is.

### Removed

- `fs::BabelFS`, `fs::ReactFS`, `fs::JasmineFS` and their generated headers (`BabelData.hpp`, `ReactRuntimeData.hpp`,
  `Jasmine{Js,Html,Css,Boot,Favicon}Data.hpp`, ~2.1 MB), `third_party/{babel,react,jasmine}`, `js/developer/vendor/update.mjs`
  and `check.mjs`, their `*AssetFresh` tests, `EmbeddedGzipContents` and the `@babel/standalone` dependency (#312).
  Migration: `wf install` the package you need and `wf embed` a provider for your build output. The HTTP encoding tests use
  a neutral gzip fixture.
- **Breaking:** the terminal component and the `wf` tool manager (#313): `webfront::Terminal`, `terminal::Shell`,
  `webfront::ToolManager`, `fs::TerminalFS`, `fs::ToolManagerFS`, `fs::ToolStore`, `fs::PackageFS`, their embedded
  modules, the embedded xterm.js copy, `third_party/xterm` and `cmake/UpdateXterm.cmake`. The developer environment's
  terminal and `webfront::Commands` replace them; `docs/UI_COMPONENTS.md` describes the migration. xterm.js is a
  locked dependency of the environment archive. `fs::tools::safeRelativePath` becomes `fs::safeRelativePath`
  (`<webfront/system/SafePath.hpp>`).

### Changed

- Guard of the minimal core (#314): the `MinimalCoreAssets` CTest case lists the generated resource headers of
  `include/webfront/` and fails on one outside the normative list of ADR-016 or when the decoded payloads exceed 150 KiB
  (currently 35 766 bytes in 99 341 bytes of headers; `include/webfront/system` went from about 2.5 MB at the start of #303 to 188 KB).
  The default favicon is the source `js/core/favicon.ico` rendered by `js/core/EmbedBinaryAsset.cmake`, pure CMake with
  no Node.js, into `FaviconData.hpp` (`FaviconAssetFresh` checks it); it is no longer a hand-written gzip array in
  `IndexFS.hpp` and is served unencoded. `IndexFS::WebFrontIco` stays an alias of the generated struct.
- The browser integration suite `test/web/` runs through `wf test` (#311): `test/web/suite/` is a WebFront project
  (`jasmine-core`, `react`, `react-dom` from the local registry), `webtest` is the `webfront-wf` driver with the
  callbacks of the specs (`test/driver/WfDriver.hpp`), and the Signal Studio page is a JSX module built by esbuild-wasm.
  `SpecRunner.html` and the `TestFS` composition of `JasmineFS`, `ReactFS` and `BabelFS` are removed from the test. The
  Jasmine CTest case needs `WEBFRONT_DEVELOPER_FIXTURES` (otherwise only the lifecycle and engine cases are registered);
  the pinned bootstrap environment runs it.
- **Breaking:** `BasicWF::getLink(id)`, whose reference could outlive a released connection, is replaced by
  `findLink(id)`, which shares the link with the caller (null once the browser is gone).
- `DeveloperBootstrapOptions::registry` is now `std::optional<std::string>`: unset, the manifest's registry
  applies; set, it must be HTTPS or a same-origin path and takes precedence over the manifest. The environment
  configuration gains `registryExplicit`; earlier environments keep reading `registry`.

- **Breaking:** library headers moved to `include/webfront/`; include them as `<webfront/WebFront.hpp>`,
  `<webfront/http/...>`, `<webfront/system/...>` and so on. The former spellings (`<WebFront.hpp>`,
  `<http/...>`, `<system/...>`) are removed with no redirect headers, so these generic names no longer
  risk colliding with other libraries; point consumers at `include/` and update their includes.
  `include/WebFront220128.hpp.gz` and the historical `webfront/` prototype are removed.
- Repository root reduced to `.github`, `cmake`, `docs`, `examples`, `include`, `js`, `test` (and `third_party`
  until #319). `assets/` and `tools/` become `js/core` and `js/developer`; `webtest/` becomes `test/web`,
  `fuzz/` becomes `test/fuzz`, unit tests move to `test/unit`; `CHANGELOG.md`, `Doxyfile.in` move to `docs/`,
  `CONTRIBUTING.md` and `codecov.yml` to `.github/`, `Dockerfile` to `.devcontainer/`. Moves keep Git history.
- Remove the CodeScene configuration and the Codecov upload (`.github/codecov.yml`, `CODECOV_TOKEN`); the Linux job still prints an lcov summary.
- Developer distribution 0.4.1 (published as `developer-v0.4.1`): generated providers include
  `<webfront/system/FileSystem.hpp>`. Published 0.4.0 remains immutable.

### Fixed

- `runNonInteractive` (developer environment 0.6.1) kept only the summary of a command: `output += await execute(...)` read
  `output` before the command ran, so what it wrote meanwhile (a script's console output, install progress) was lost on
  success. Regression test `WfDriverScriptOutput`.

- A browser's disconnection is now reported and its connection released: `WebLink` emits its `closed` event once when
  the browser closes the connection or it fails. The event was never emitted before, so nothing observed
  disconnections and every closed connection (socket, buffers) stayed until the application was destroyed. `BasicWF`
  now releases it by a task on the server context; afterwards untyped `jsFunction` calls to that browser are
  dropped, typed ones fail and `jsFunctionSender` reports an error when started, instead of throwing
  `std::out_of_range`.
- Linux CEF builds: the runtime files, resources and locales are copied once per destination directory by a shared
  copy target, instead of by every CEF target's post-build step, which made parallel builds fail intermittently
  ("Error copying directory ... locales"). The destination is the target's output directory
  (`RUNTIME_OUTPUT_DIRECTORY`, initialized from `CMAKE_RUNTIME_OUTPUT_DIRECTORY`); a per-configuration output
  directory, or one set after the frontend setup, is an explicit configure error. A build with nothing new copies
  nothing.
- CI installs CMake from the Kitware release archive checked against a pinned SHA-256
  (`.github/actions/setup-cmake`), instead of an action that lists the releases through the GitHub API and failed
  intermittently ("Unable to find version matching").

## 0.4.0 - 2026-10-21

### Changed

- Move the public examples to `examples/` and retain only Hello World and Game of Life.
  Readable library web resources now live in `assets/`; regression fixtures remain under tests/tools.
- Rework the README around a minimal Hello World, an early Life thumbnail and a step-by-step
  bootstrap, dependency-installation, preview and embedded-distribution walkthrough.
- Combine per-change CI into three native desktop jobs with caches and cancellation.
  GCC coverage stays in the Linux job on every PR. Extended CEF, sanitizer and fuzz checks run weekly or manually.
- Run `MddlogIntegration.yml` source/installed integration and sanitizer qualification weekly or manually.
- Developer distribution 0.4.0 is published (`developer-v0.4.0`) and pinned by `app.bootstrap("developer")`.
- No new library functionality.

## 0.3.0 - 2026-10-21

### Added

- `std::bitset<N>` callback arguments and results, C++ call arguments and typed future/sender results cross the bridge as packed `Uint8Array` bytes (bit `i` in byte `i / 8`, least-significant first), with exact-length and zero-padding validation. A packed Life demonstration exchanges 128 bytes per generation.
- Opt-in developer environment: `NativeApp::bootstrap("developer")` downloads a SHA-512-pinned environment (distribution 0.3.1 on GitHub Pages) whose terminal offers `wf create|install|list|preview|embed`. It resolves npm packages in the browser, locks and re-verifies them, compiles with esbuild-WASM, previews in an iframe and generates a gzip-compressed C++ resource header. The bridge callbacks `webfront.developer.*` publish files through staged, bounded, transactional writes confined to the explicit project directory; abandoned transactions expire and stale staging is purged at startup. WebSocket clients without an `Origin` are refused.

- `webfront::Task`, `app.spawn(task)` and `ui.spawn(task)` launch application-owned coroutines without execution plumbing; a returned `TaskHandle` cancels an individual task. `app.shutdown()` joins owned work explicitly. Spawning during stop/shutdown returns a stopped handle when rejected; destructor joins log and wait for stalled external work instead of propagating drain errors. `jsFunctionSender(name)` defaults to `void`. `WebFrontAwaitBrowser` is a compiled example matching the README.

- `ui.jsFunctionSender<R>(name)(...)` returns a lazy sender that can be awaited in a task and cancelled through its receiver's stop token. Cancellation stops the local wait; dispatched JavaScript continues. Existing `std::future` calls remain available.

### Changed

- **Breaking (low-level API):** Server shutdown joins a counting scope covering HTTP and WebSocket tasks. Low-level `WebSocket::create` and `WebLink` constructors now require an external `networking::TaskScope&` whose owner drains it before releasing dependencies.

- JavaScript bridge arguments or results outside the requested non-boolean C++ integer range, including NaN and infinities, now fail decoding instead of triggering undefined conversion behavior. A JavaScript call rejects its Promise; a typed C++ call completes its future exceptionally.
- Tests enable vendor standard-library hardening and, when supported, pattern initialization. Optional sanitizer builds and bounded decoder fuzzing remain private to internal targets.
- Demo and browser-test frontend selection, SDK availability and test wiring share a CMake registry; existing selector names and defaults are preserved.
- `WEBFRONT_EMBED_CEF=ON` now fails during configuration outside Windows, Linux and macOS (`Darwin`), before downloading dependencies. This includes FreeBSD configurations that previously reached dependency setup.
- CEF system include directories derived from `CEF_ROOT` are added only when `WEBFRONT_EMBED_CEF` is enabled, rather than whenever the directory exists.
- CEF's `BUILD_TESTING=OFF` setting is scoped to its dependency through CPM. Other inherited CEF configuration flags forced into the cache remain unchanged.
- `BUILD_TESTING` is no longer overwritten globally, including in CEF-off builds. When testing is enabled, CTest can now use its normal `ON` default and generate dashboard targets (`Experimental`, `Nightly`, `Continuous`) and `DartConfiguration.tcl`; callers can set `BUILD_TESTING` explicitly.
- CEF setup no longer writes late macOS patches or an empty wrapper stub into dependency sources. Upstream demo targets are excluded from the default build, and a missing or non-static `libcef_dll_wrapper` fails during configuration.
- Browser selectors retain their cached choice on reconfiguration. CEF runners on Windows/macOS now warn that no browser test is registered; set `WEBFRONT_WEBTEST_FRONTEND=WebView2` explicitly to switch a Windows runner.

### Fixed

- HTTP requests retain their own partial method name when receives from multiple connections interleave.
- Input decoding no longer indexes past empty wire strings or unknown HTTP method names, and rejects overflowing HTTP version fields.
