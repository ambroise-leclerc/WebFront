# WebFront contributor and agent guide

## Project goal and current maturity

WebFront is a C++23 header-only library for serving a browser UI from a C++ application. It embeds an HTTP/WebSocket server and exposes an asynchronous binary bridge for calls in both directions. The two directions are not symmetric:

- JavaScript calls registered C++ callbacks with `webFront.cppFunction(name)(...)`. This always returns a Promise, even for a `void` C++ callback, and the Promise rejects on failure, so an ignored Promise can cause an unhandled rejection.
- C++ calls browser functions with `ui.jsFunction(name)(arguments...)`, which is fire-and-forget and ignores the result, or with `ui.jsFunction<R>(name)(arguments...)`, which returns a `std::future<R>`. Only the untyped C++ form is fire-and-forget.
- `webFront.ready` resolves after the WebSocket handshake acknowledgement and rejects if the initial connection fails or closes.

The project is an experimental foundation, not a production-ready UI framework. The newcomer path is `webfront::WebFront app; app.show();` (examples `HelloWorld` and `WebFrontGreeter`); the minimum automated demonstration is the native ES-module example plus the Jasmine browser integration test. General DOM abstractions and cancellation of outstanding results are future work.

## Repository map

- `include/`: public header-only library. `WebFront.hpp` is the main orchestration API.
- `include/http/`: HTTP and WebSocket protocol implementation.
- `include/weblink/`: bridge message encoding and routing.
- `include/system/`: layered virtual filesystems and embedded web resources.
- `include/frontend/`: system-browser and CEF frontend adapters.
- `include/components/`: opt-in UI components (`Terminal.hpp`, `ToolManager.hpp`); their assets are served by embedded providers such as `include/system/TerminalFS.hpp`, and installed tools by `include/system/PackageFS.hpp` from `ToolStore.hpp`'s project directory.
- `src/`: example applications and browser assets: `HelloWorld.cpp` (no assets, embedded fallback page), `Greeter.cpp` with `greeter/` (editable UI), `ModuleDemo.cpp` with `module-demo.html` and `react.html`, `Terminal.cpp` (terminal component demo), and the readable embedded sources `WebFront.js`, `fallback/` and `components/`.
- `test/`: Catch2 unit tests.
- `webtest/`: Jasmine real-browser integration test.
- `cmake/` and `.github/workflows/`: build support and CI.
- `third_party/xterm/`: vendored xterm.js distribution embedded by the terminal component; refresh it only with `cmake/UpdateXterm.cmake`.
- `docs/PROJECT_STATUS.md`: evidence-based status snapshot and known limitations.
- `docs/UI_COMPONENTS.md`: UI component architecture, the terminal component and the planned `wf` tool manager.
- `docs/ISSUE_AUDIT.md`: recommendations for the current GitHub backlog.

The legacy `webfront/`, `tests/`, and `webtest` directories are not interchangeable: use `test/` for current C++ tests and `webtest/` for browser tests. Do not revive legacy code without a specific issue and regression coverage.

## Build and test commands

Use out-of-source builds. CPM downloads dependencies during first configure, so network access may be required.

```bash
# Newcomer quick start: no options, no CEF, no tests; builds HelloWorld, WebFrontGreeter, WebFrontApp
mkdir build
cd build
cmake ..
cmake --build .
./src/HelloWorld
./src/WebFrontGreeter

# Fast, portable C++ baseline (no CEF download)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Run one Catch2 scenario directly
./build/test/tests "Scenario: JsFunction"

# Linux real-browser integration test
cmake -S . -B build-cef -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=ON -DENABLE_TESTING=ON
cmake --build build-cef --target webtest --parallel
ctest --test-dir build-cef -L web-integration --output-on-failure

# Manual native-module demo (opens the system browser unless the source selects CEFFrontend)
./build/src/WebFrontApp

# Optional React example
./build/src/WebFrontApp react.html
```

Important CMake options:

- `WEBFRONT_EMBED_CEF=OFF` (default): CEF is unavailable; this is the normal unit-test build. `ON` downloads/links CEF and enables the automated browser test; it makes `CEFFrontend` available but does not select it: `webfront::WebFront` always uses the system browser.
- `ENABLE_TESTING=ON` (default `OFF`): build Catch2 and Jasmine test targets. CI and contributor commands must pass it explicitly.
- `WEBFRONT_BUILD_EXAMPLES` (default `ON` only for the top-level project): build the examples. As a subproject (`add_subdirectory`), link `WebFront::WebFront`; no examples, tests or global compiler settings are added.
- `ENABLE_COVERAGE=ON`: add GCC/Clang coverage instrumentation to the C++ tests.

If the environment provides a read-only default ccache directory, set `CCACHE_DIR` to a writable temporary directory for local builds. Do not encode machine-specific cache paths in project files.

## Architecture and invariants

`BasicWF<NetProvider, Filesystem, Policy, Frontend>` owns the HTTP server, connected `WebLink` objects, registered C++ callbacks, and server thread. `BasicUI` identifies one connected browser. `onUIStarted` sets the application's single handler for connected browsers; reusable code must use `subscribeUIStarted`, which adds a handler for as long as the returned `UISubscription` lives (the handle stays safe after the application is destroyed). A callback whose first parameter is the application's `UI` type receives the calling browser instead of reading it from the call data. A frontend must satisfy `frontend::FrontendType` and decides whether the server stops when `open()` returns.

The HTTP server connections and the WebSocket read/write loops are coroutines (`networking::Task`) awaiting the senders a network provider returns (`Receive`, `Send`, `Accept`; `networking::TCPStdnet` forwards them to stdnet). A stop request cancels them from any thread and they unwind on the io_context thread, so I/O objects are never closed from another thread. Whoever owns the objects the tasks use must run `Server::drain()` before destroying them (`~BasicWF` does).

`webfront::WebFront` is default-constructible (`WebFrontConfig`: loopback address, port `9002`, no document root) and `show(page = "index.html")` opens a page. Without a document root no local file access happens; `WebFrontConfig::documentRoot` serves local files for development only, after the embedded `WebFront.js` (which a root can never override) and before the embedded fallback page.

Filesystems implement `open(std::filesystem::path) -> std::optional<fs::File>` and are composed with `fs::Multi<>`; the first provider returning a file wins. `NativeDebugFS` is development-only and must not be presented as a production sandbox.

Bridge calls are asynchronous, and the two directions differ. `webFront.cppFunction(name)(...)` always returns a JavaScript Promise (also for `void` callbacks), so tests and examples must await or handle it. On the C++ side, only the untyped `ui.jsFunction(name)(...)` is fire-and-forget; `ui.jsFunction<R>(name)(...)` returns a `std::future<R>`. On missing functions, callback exceptions, malformed returns, and disconnects, a C++ future completes exceptionally (`get()` throws) and a JavaScript Promise rejects. Result types are limited to the bridge's scalar, string, tuple, and numeric typed-array types. Strings carry a 16-bit length: a longer one throws `std::length_error` when C++ encodes it, which rejects the Promise when it is a callback's result, and a `RangeError` in JavaScript. Callback signatures are deduced for ordinary lambdas, function pointers, and `std::function`; generic lambdas and overloaded call operators need explicit `cppFunction<R, Args...>`. Do not write tests or documentation that present JavaScript calls as fire-and-forget, imply an untyped C++ call returns a value, or suggest that richer objects cross the bridge. Registered callback objects must outlive their registration through owned storage; never capture a forwarding-reference parameter by reference in a stored handler.

The embedded `WebFront.js` wire format and C++ `Messages.hpp` implementation must evolve together. Any protocol change requires encoding/decoding unit tests and a real-browser regression test.

## Coding and testing conventions

- Use C++23, four-space indentation, the repository `.clang-format`, and the naming rules in `CONTRIBUTING.md`.
- Keep production implementation header-only under `include/`; `src/` contains examples only.
- Prefer standard-library facilities and explicit ownership. Keep public headers ODR-safe (`inline`/templates as appropriate).
- Catch2 tests use `SCENARIO`/`GIVEN`/`WHEN`/`THEN` and deterministic mocks.
- Jasmine tests use native modules and must report completion to C++, close themselves, return a meaningful process status, and have a CTest timeout.
- Every bug fix needs a failing regression test at the lowest useful level. Bridge changes also need the browser integration test.
- Tests must not require user input, a fixed pre-existing browser session, or public network access after dependencies are configured.

Before declaring work complete, run the CEF-off build and CTest suite. Run the CEF/Xvfb test when changing the bridge, browser lifecycle, module serving, Jasmine assets, or CEF frontend. Update README/status documentation when commands, public behavior, or limitations change. The `NewcomerDocsMatchExamples` CTest case fails when the README quick-start commands or snippets drift from the examples, CMake targets, or CI commands, and `AddSubdirectoryConsumer` builds a minimal parent project linking `WebFront::WebFront`.

## Git and workspace safety

The workspace may contain user-owned untracked documents and large build directories. Never delete, move, format, stage, or overwrite unrelated files. In particular, names such as `A_faire*`, `Texte*`, calendar SVGs, `data/`, local batch files, and existing `build*` trees are not automatically project work.

Inspect `git status --short` before and after changes. Preserve overlapping user edits and use targeted patches. Never use destructive reset/checkout commands. Feature branches follow the issue-number convention and target `develop`; `master` is the release branch. GitHub issue changes require explicit user authorization and evidence from code/tests/history.

Never add AI-assistance attribution or advertising anywhere: not in commits, PR titles or descriptions, issue or PR comments, reviews, or documentation. This holds even when a tool, harness or template offers a "Generated by ..." footer or a session link; omit it, and strip it if a server appends it. Commits and pull requests must never contain AI-assistance attribution. Do not add `Co-authored-by` trailers for an AI tool, generated-by notices, bot attribution, or statements crediting Codex, Claude, Copilot, Vibe, or another assistant in commit messages, commit trailers, PR titles, or PR descriptions.

## Definition of done

A change is done only when its intended behavior is demonstrated, relevant unit and integration tests pass, failure paths return nonzero or throw as documented, CI/build instructions match reality, public limitations are not overstated, and the final diff contains no unrelated workspace files.
