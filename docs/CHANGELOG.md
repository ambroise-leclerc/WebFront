# Changelog

All notable changes to WebFront, newest first. The format follows [Keep a Changelog](https://keepachangelog.com/) and
[Common Changelog](https://common-changelog.org/); see [Changelog format](../.github/CONTRIBUTING.md#changelog-format).

Each version opens with a short summary, then lists `Breaking`, `Added`, `Changed`, `Fixed` and `Security` entries
(empty categories are omitted). An entry is one line that says what changes for you and ends with its references.
Mechanisms, error codes and platform details live in the cited ADR, guide or pull request. The earlier, detailed text of
these entries stays in the GitHub releases and the Git history.

## Unreleased

### Changed

- Open the developer page on the interface, over the whole window, once the project has a preview; the terminal moves to a floating, movable window behind a tab on the right edge that shows the session's state, and keeps its session when collapsed. Its shortcut is F9, or the manifest's new optional `developer.terminalShortcut`. Environment sources 0.9.1, not published yet (#539, `docs/DEVELOPER_ENVIRONMENT.md`).
- Run the unit tests as a few processes instead of one per scenario with `-DWEBFRONT_TEST_UNIT_SHARDS=<n>`: each of the `n` CTest cases is a Catch2 shard that reports every scenario in `unit-results-<shard>.xml`. The default still registers one CTest case per scenario; the desktop CI jobs pass 4 and no longer run the fast tier twice on a pull request (#543).

### Fixed

- Build with `-DWEBFRONT_EMBED_CEF=ON` on Linux aarch64 instead of failing on `-m64`, and cross-compile CEF: WebFront pins CEF 156.0.28, whose package selects the archive and `PROJECT_ARCH` from the target (#523).

## [0.9.0] - 2026-11-10

Last minor revision before 1.0, with the developer environment at the same number. It lays the groundwork for the
system services of [ADR-020](adr/README.md): confined writable storage, native window access, an interface-thread executor,
ranged and dynamic HTTP routes. **One migration:** libraries and developer environments older than 0.6.5 must be rebuilt,
because they pin a distribution address that no longer bootstraps. Developer environment 0.9.0 is the first one published
by the `release-developer` workflow.

### Breaking

- Rebuild libraries and developer environments older than 0.6.5 against current sources: they pin the former distribution address, which redirects to `https://dist.webfront.fr/` without the CORS headers a browser needs, so `bootstrap("developer")` no longer starts (#478, ADR-021).

### Added

- Send long strings, homogeneous lists of strings, tuples, typed arrays and bitsets through the bridge, raise typed errors with application codes, and let a `cppFunction` callback return a `webfront::ResultTask<R>` or another sender (#456–#459, ADR-003, `docs/ASYNC_EXECUTION.md`).
- Add `fs::Storage`, `fs::NativeStorage` and `fs::MemoryStorage`: writable storage confined to declared roots, with atomic commits (#460, ADR-002).
- Add `frontend::NativeWindow` and `UI::nativeWindow()`: typed access to the native window of an embedded view (#463, ADR-020).
- Add `frontend::UIExecutor` and `BasicWF::uiExecutor()`: a sender that completes on the interface thread, for every frontend (#462, ADR-007).
- Add `BrowserWithUILoopFrontend`: the system browser with an optional minimal interface loop (#462).
- Add `app.addRoute(prefix, handler)`: dynamic `GET`/`HEAD` routes with streamed bodies (#454).
- Add `Range` support (`206`, `416`) for files of known size (#455).
- Add `app.addReadRoute`, `ui.grantRead` and `webFront.bound`: HTTP reads bound to one browser client (#469, ADR-020).
- Add `UI::isEmbeddedView()` and `UI::isLocalClient()`: a connection proves it comes from the embedded view (#453, ADR-020).
- Support `alert`, `confirm` and `prompt` in WKWebView, and `<input type="file">` and downloads in the embedded frontends (#451, #452).
- Add `webfront::automation::enableAutomation(app, options)`, `webfront-agent --list`, `--print-mcp-config` and the `call` operation, to drive an application's own window (#501, ADR-019).
- Add the `@webfront/services` package skeleton and `wf run services-package` (#461, ADR-010).
- Add the `release-developer` workflow that builds, tags and publishes the developer environment (#482, ADR-021).

### Changed

- Serve the developer distribution from `https://dist.webfront.fr/` (#478, ADR-021), as the breaking entry above explains.
- Stream unencoded files of known size instead of loading them whole (#454).
- Make `wf run pin` fail when the pinned URL leaves `baseUrl`'s origin, redirects or lacks CORS (#482).
- Make `wf check` without a `tsconfig.json` root the project's own `.d.ts`, `.d.mts` and `.d.cts` files (#501).
- Make `safeRelativePath` refuse the device names Windows reserves, and names ending with a dot or a space (#460).
- Diagnose asynchronous media and file-services specs before Jasmine's generic timeout (#504).

### Fixed

- Stop `Server::drain()` and `shutdown()` from throwing `std::logic_error` while a route handler is running (#469).
- Build the unit tests with GCC 16.1 and make the native storage tests independent of the umask (#517).

### Security

- Refuse a read grant to any connection that is not the embedded view, unless `WebFrontConfig::remoteFileTransport` declares the transport (#469, ADR-020).
- Refuse a root or directory that another POSIX user could move (`unsafeDirectory`), and a link or reparse point inside a storage root (#460, ADR-002).

## [0.8.0] - 2026-11-01

Automation, consumer tests and embedded engines. **No breaking change.**

### Added

- Add `PROGRAMS` and `webFrontHost.run(...)` to run authorized native programs from the specs of `webfront_add_jasmine_test` (#445).
- Add `makeTemp`, `withTemp`, `writeFile` and `readFile`: private temporary spaces for those specs (#445).
- Add `webFrontCapture.screenshot(name)` and `record(name, options)`: PNG captures of the web view (#468).
- Add `webfront::embeddedFrontendAvailable` and `webfront::EmbeddedWebFront`, usable without consumer-side macros (#467).
- Make `init`, `create`, `install`, `restore` and `embed` available to `webfront-agent`, each behind its own `--allow` grant (#466).

### Changed

- Fail at startup when an older environment is granted an operation it lacks, naming the operation (#466).

## [0.7.0] - 2026-10-31

Test and CI housekeeping, with one **breaking** change to CMake option names.

### Breaking

- Prefix WebFront's test, fuzzing, coverage and sanitizer CMake options with `WEBFRONT_`, for example `WEBFRONT_ENABLE_TESTING`. Unprefixed names and old cache entries no longer configure WebFront (#412).

### Changed

- Cut pull-request CI time: four-file unity batches, no example or MSVC analysis on pull requests into `develop` (#435, `docs/BUILD_TIME_STUDY.md`).
- Include `developer_integration` in the documented Linux CEF build commands (#434).

### Fixed

- Remove the Windows agent's native profile after a forced exit (#439).

## [0.6.0] - 2026-10-31

Release of the DOM, forms, React and automation layers, with **two breaking changes** to the C++ API: values of `UI`,
`JsFunction` and `JsFunctionSender` can no longer be built by hand, and `BasicWF` is no longer copyable or movable.
The largest version of the pre-1.0 series, condensed from 49 entries.

### Breaking

- Use the `UI` that `onUIStarted`, `subscribeUIStarted` or a callback hands over instead of constructing `UI`, `JsFunction` or `JsFunctionSender` yourself: their public constructors take the connection generation as an extra argument (#428).
- Do not copy or move a `BasicWF`: it is now non-copyable and non-movable, which the pointer held by `UI` requires (#400).

### Added

- Add `ui.document()`: per-browser DOM islands with batched writes and awaitable events (#380, #381, #384, `docs/DOM.md`).
- Add type descriptors and generated forms and inspectors, with an optional C++26 reflection layer (#383, `docs/FORMS.md`).
- Add the `@webfront/react` adapter: coalesced C++ stores and `Island` (#382, `docs/REACT.md`).
- Add `app.broadcast(name, options)(...)` and `app.forEachUI(function)` to reach every browser (#401).
- Add `webfront-agent`, local automation of the developer window through stdio MCP and a JSON CLI (#403, ADR-019, `docs/AUTOMATION.md`).
- Add consumer Jasmine/CTest integration with `webfront_add_jasmine_test` (#402, `docs/CONSUMER_TESTING.md`).
- Add `frontend::nativeWebViewAvailable` and `frontend::NativeOrBrowserFrontend` (#398).
- Take the native window's title and initial size from `WebFrontConfig::window`.
- License WebFront under Apache-2.0 OR CECILL-B, at the recipient's option.

### Changed

- Make `BasicUI` (`UI`) an assignable value type that holds a pointer to the application (#400).
- Enable native-window detection by default for `add_subdirectory` and CPM consumers (#397).

### Fixed

- Reject a stale `UI` after its connection identifier is reused: calls are dropped and typed calls fail (#428).
- Keep automation shutdown and cleanup reliable on Windows and under load (#416, #439).

## [0.5.0] - 2026-10-26

The developer environment becomes a complete, standalone `wf` toolchain, and the library moves under `include/webfront/`.
Three **breaking** changes. Condensed from 62 entries.

### Breaking

- Include headers as `<webfront/WebFront.hpp>`, `<webfront/http/...>` and so on. The former spellings are removed with no redirect headers (ADR-016).
- Replace `BasicWF::getLink(id)` with `findLink(id)`, which shares the link and is null once the browser is gone.
- Remove `webfront::Terminal`, `terminal::Shell`, `ToolManager` and the related filesystems; use the developer environment's terminal and `webfront::Commands` (#313).

### Added

- Add `wf run`, `exec`, `check`, `test`, `build`, `dev` and `restore`, and the `webfront.project.json` manifest and lock (#308–#310, `docs/DEVELOPER_ENVIRONMENT.md`).
- Add `webfront-wf`, the non-interactive driver, so tests, regeneration and CI need no JavaScript runtime or package manager (#318, #321).
- Make the developer environment build itself (#315).

### Changed

- Guard the minimal core's embedded assets with `MinimalCoreAssets` (#314, ADR-016).
- Run the browser integration suite through `wf test` (#311).
- Remove vendored `third_party/` sources and the retired PI starter (#319).

### Fixed

- Report a browser's disconnection and release its connection (`WebLink` emits `closed` once).
- Stop the bridge encoder from writing diagnostics on every message (#363).

## [0.4.0] - 2026-10-21

Repository and CI reorganisation. **No new library functionality, no breaking change.**

### Changed

- Move the public examples to `examples/`, keeping Hello World and Game of Life.
- Rework the README around a minimal Hello World and a step-by-step developer walkthrough.
- Combine per-change CI into three native desktop jobs; extended CEF, sanitizer and fuzz checks run weekly.

## [0.3.0] - 2026-10-21

Coroutines and the opt-in developer environment. One **breaking** change to the low-level API.

### Breaking

- Pass an external `networking::TaskScope&` to the low-level `WebSocket::create` and `WebLink` constructors, and drain it before releasing their dependencies. Server shutdown now joins a counting scope.

### Added

- Add `std::bitset<N>` arguments and results, packed as `Uint8Array`.
- Add `NativeApp::bootstrap("developer")`, the opt-in developer environment.
- Add `webfront::Task`, `app.spawn(task)` and `ui.spawn(task)` for application-owned coroutines.
- Add `ui.jsFunctionSender<R>(name)(...)`, a lazy sender cancellable through its receiver's stop token.

### Changed

- Fail decoding when a bridge integer is out of the requested C++ range, instead of invoking undefined behavior.

[Unreleased]: https://github.com/ambroise-leclerc/WebFront/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.2.0...v0.3.0
