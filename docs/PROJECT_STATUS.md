# WebFront project status

Status snapshot: 2026-07-19. Optional mddlog integration note: 2026-10-02.

The default build remains header-only. `WEBFRONT_USE_MDDLOG=ON` adds a WebFront-owned
compiled diagnostic adapter while keeping consuming translation units module-free.
See [the optional integration contract](mddlog-integration.md) for dependencies,
qualified tools and verification. With the option on, browser logs travel through
mddlog's bounded transport consumer and WebLink captures link/call context. This
option does not accept mddlog ADR-003.

## Intended product

WebFront is a C++23 header-only experiment for browser-based C++ user interfaces. A C++ process serves local HTML/JavaScript and exchanges typed function-call messages with the page over WebSocket. It supports an embedded CEF frontend or the system browser.

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
- Registered C++ callbacks own their callable safely. Calls remain asynchronous and return no value across the bridge.
- Catch2 covers `.mjs` MIME handling.
- Jasmine native-module specs report their final status to C++. The executable verifies both bridge directions, asks JavaScript to close the CEF window, and returns nonzero on any failed condition.
- The browser integration is a timeout-bounded CTest test for CEF-enabled Linux builds and runs under Xvfb in a focused CI job.
- The bridge carries all JavaScript numeric typed-array variants in both directions. C++ `std::vector`, `std::array`, and `std::span` values use the matching typed-array encoding, while decoded parameters own their storage.
- `src/WebFront.js` is the readable browser bridge source. CMake deterministically regenerates the embedded `include/system/WebFrontJsData.hpp` asset and CTest rejects a stale generated copy.
- Text assets are embedded **verbatim rather than compressed**, so `HTTPServer` serves them without a `Content-Encoding` header. Deflate streams are not reproducible across the zlib builds on our macOS, Linux, and Windows runners, so a pre-compressed blob could not be verified by the staleness check. Restoring compression — either a reproducible build-time gzip or on-the-fly compression keyed on `Accept-Encoding` — is tracked as a follow-up. The fallback HTML and module have readable sources under `src/fallback/`; `favicon.ico` remains Brotli-encoded. Regenerate the text headers with `cmake --build build --target webfront-embedded-assets`. Sources and generated headers use LF on every platform so the CTest freshness checks compare identical bytes.
- The fallback page awaits `webFront.ready` to display the initial bridge connection result. This promise resolves after the handshake acknowledgement and rejects if the initial connection fails or closes; it does not track later disconnects. Jasmine exercises failure against a test-owned HTTP server on an OS-assigned loopback port that rejects WebSocket upgrades, including the case where the application never consumes `ready`.
- WebSocket writes own their payload buffers until asynchronous completion and are serialized per connection.
- Correlated result messages now support asynchronous C++ futures and JavaScript promises, including void completion, missing-function errors, callback exceptions, malformed-return rejection, and disconnect rejection. Untyped calls remain fire-and-forget.
- `AGENTS.md` is the canonical development guide for supported coding agents.
- The default frontend always uses the system browser, including CEF-enabled builds. Applications opt into embedded windows through
  `WebFrontWithFrontend<frontend::CEFFrontend>`; selecting CEF without compiled support throws during construction. On macOS,
  executable discovery and framework paths follow the running target and CMake's `../Frameworks` deployment layout.

Local verification on 2026-07-19 completed successfully: the CEF-off build passed all 47 CTest cases, including deterministic asset and typed-array protocol coverage, and a CEF-enabled build passed `WebFrontBrowserIntegration` under Xvfb. The browser test exercised every supported numeric typed array from C++ to JavaScript and JavaScript to C++, ordinary tuple arrays, final Jasmine reporting, and automatic CEF shutdown.

## Known limitations and next decisions

- Result values are currently limited to the bridge's existing scalar, string, tuple, and numeric typed-array parameter types; richer object/DOM values remain out of scope.
- The public API does not yet expose cancellation for an outstanding future or Promise.
- Ordinary JavaScript arrays encode as tuple-like parameters; only numeric typed arrays use the homogeneous array wire representation.
- `NativeDebugFS` exposes development files and is not a production packaging format.
- CEF is a large optional dependency. Only the focused Linux integration job should download it for the minimal baseline.
- React/Babel assets remain examples rather than the core demonstration. Modern Node/Vite integration, WebView2, overlays, and packaged filesystems require separate product decisions.
