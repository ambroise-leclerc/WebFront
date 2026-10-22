# Changelog

## Unreleased

### Added

- Project manifest `webfront.project.json` (format 1: `name`, `version`, `dependencies`, `registry`, `scripts`,
  `entries`, `providers`, `test`, `permissions`), strictly validated; `wf init`, `wf install [--update]` and
  manifest-driven `wf preview`/`wf embed` in developer distribution 0.5.0 (not published yet; the pin in
  `DeveloperReleaseData.hpp` stays on 0.4.1 until 0.5.0 is published and verified). The lock records the digest of
  the manifest dependencies and reports a desynchronized state. `developer::Store::commitGroup` publishes
  manifest and lock through a synced intent journal with crash recovery and a concurrent-install precondition.
  New bridge callbacks `webfront.developer.state` and `webfront.developer.commitGroup`. Starters ship a manifest;
  `wf install` now needs `wf init` first, which adopts the roots of an existing lock so a project locked before 0.5.0
  keeps its locked graph. A committed publication that cannot be completed does not prevent opening the project:
  it is reported (`developer::IncompletePublication`) and completed by the next command.

### Changed

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
- Opt-in browser-powered developer environment: `NativeApp::bootstrap("developer")` downloads a SHA-512-pinned environment (distribution 0.3.1 on GitHub Pages) whose terminal offers `wf create|install|list|preview|embed`. It resolves npm packages in the browser, locks and re-verifies them, compiles with esbuild-WASM, previews in an iframe and generates a gzip-compressed C++ resource header. The bridge callbacks `webfront.developer.*` publish files through staged, bounded, transactional writes confined to the explicit project directory; abandoned transactions expire and stale staging is purged at startup. WebSocket clients without an `Origin` are refused.

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
