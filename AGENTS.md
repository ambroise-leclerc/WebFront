# WebFront contributor and agent guide

## Project goal and current maturity

WebFront is a C++23 header-only library for serving a browser UI from a C++ application. It embeds an HTTP/WebSocket server and exposes a binary bridge for fire-and-forget calls in both directions:

- JavaScript calls registered C++ callbacks with `webFront.cppFunction(name)`.
- C++ calls browser functions with `ui.jsFunction(name)(arguments...)`.

The project is an experimental foundation, not a production-ready UI framework. The minimum supported demonstration is the native ES-module example plus the automated Jasmine browser integration test. Function return values, remote exception propagation, and general DOM abstractions are future work.

## Repository map

- `include/`: public header-only library. `WebFront.hpp` is the main orchestration API.
- `include/http/`: HTTP and WebSocket protocol implementation.
- `include/weblink/`: bridge message encoding and routing.
- `include/system/`: layered virtual filesystems and embedded web resources.
- `include/frontend/`: system-browser and CEF frontend adapters.
- `src/`: example applications and browser assets. `module-demo.html` is the minimal demo.
- `test/`: Catch2 unit tests.
- `webtest/`: Jasmine real-browser integration test.
- `cmake/` and `.github/workflows/`: build support and CI.
- `docs/PROJECT_STATUS.md`: evidence-based status snapshot and known limitations.
- `docs/ISSUE_AUDIT.md`: recommendations for the current GitHub backlog.

The legacy `webfront/`, `tests/`, and `webtest` directories are not interchangeable: use `test/` for current C++ tests and `webtest/` for browser tests. Do not revive legacy code without a specific issue and regression coverage.

## Build and test commands

Use out-of-source builds. CPM downloads dependencies during first configure, so network access may be required.

```bash
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

# Manual native-module demo (CEF-off opens the system browser)
./build/src/WebFrontApp

# Optional React example
./build/src/WebFrontApp react.html
```

Important CMake options:

- `WEBFRONT_EMBED_CEF=OFF`: use the system browser; this is the normal unit-test build.
- `WEBFRONT_EMBED_CEF=ON`: download/link CEF and enable the automated browser test.
- `ENABLE_TESTING=ON`: build Catch2 and Jasmine test targets.
- `ENABLE_COVERAGE=ON`: add GCC/Clang coverage instrumentation to the C++ tests.

If the environment provides a read-only default ccache directory, set `CCACHE_DIR` to a writable temporary directory for local builds. Do not encode machine-specific cache paths in project files.

## Architecture and invariants

`BasicWF<NetProvider, Filesystem, Policy, Frontend>` owns the HTTP server, connected `WebLink` objects, registered C++ callbacks, and server thread. `BasicUI` identifies one connected browser. A frontend must satisfy `frontend::FrontendType` and decides whether the server stops when `open()` returns.

Filesystems implement `open(std::filesystem::path) -> std::optional<fs::File>` and are composed with `fs::Multi<>`; the first provider returning a file wins. `NativeDebugFS` is development-only and must not be presented as a production sandbox.

Bridge calls are asynchronous and fire-and-forget. Do not write tests or documentation that imply `cppFunction()` returns the C++ result to JavaScript, or that `JsFunction::operator()` returns a JavaScript value. Registered callback objects must outlive their registration through owned storage; never capture a forwarding-reference parameter by reference in a stored handler.

The embedded `WebFront.js` wire format and C++ `Messages.hpp` implementation must evolve together. Any protocol change requires encoding/decoding unit tests and a real-browser regression test.

## Coding and testing conventions

- Use C++23, four-space indentation, the repository `.clang-format`, and the naming rules in `CONTRIBUTING.md`.
- Keep production implementation header-only under `include/`; `src/` contains examples only.
- Prefer standard-library facilities and explicit ownership. Keep public headers ODR-safe (`inline`/templates as appropriate).
- Catch2 tests use `SCENARIO`/`GIVEN`/`WHEN`/`THEN` and deterministic mocks.
- Jasmine tests use native modules and must report completion to C++, close themselves, return a meaningful process status, and have a CTest timeout.
- Every bug fix needs a failing regression test at the lowest useful level. Bridge changes also need the browser integration test.
- Tests must not require user input, a fixed pre-existing browser session, or public network access after dependencies are configured.

Before declaring work complete, run the CEF-off build and CTest suite. Run the CEF/Xvfb test when changing the bridge, browser lifecycle, module serving, Jasmine assets, or CEF frontend. Update README/status documentation when commands, public behavior, or limitations change.

## Git and workspace safety

The workspace may contain user-owned untracked documents and large build directories. Never delete, move, format, stage, or overwrite unrelated files. In particular, names such as `A_faire*`, `Texte*`, calendar SVGs, `data/`, local batch files, and existing `build*` trees are not automatically project work.

Inspect `git status --short` before and after changes. Preserve overlapping user edits and use targeted patches. Never use destructive reset/checkout commands. Feature branches follow the issue-number convention and target `develop`; `master` is the release branch. GitHub issue changes require explicit user authorization and evidence from code/tests/history.

Commits and pull requests must never contain AI-assistance attribution. Do not add `Co-authored-by` trailers for an AI tool, generated-by notices, bot attribution, or statements crediting Codex, Claude, Copilot, Vibe, or another assistant in commit messages, commit trailers, PR titles, or PR descriptions.

## Definition of done

A change is done only when its intended behavior is demonstrated, relevant unit and integration tests pass, failure paths return nonzero or throw as documented, CI/build instructions match reality, public limitations are not overstated, and the final diff contains no unrelated workspace files.
