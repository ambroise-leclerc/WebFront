# WebFront project status

Status snapshot: 2026-07-19. Optional mddlog integration note: 2026-10-02. Newcomer-first epic (#205) note: 2026-10-13. UI components note: 2026-10-14.

The default build remains header-only. `WEBFRONT_USE_MDDLOG=ON` adds a WebFront-owned
compiled diagnostic adapter while keeping consuming translation units module-free.
See [the optional integration contract](mddlog-integration.md) for dependencies,
qualified tools and verification. With the option on, browser logs travel through
mddlog's bounded transport consumer and WebLink captures link/call context. mddlog
ADR-003, which this integration implements, was accepted on 2026-10-02.

## Newcomer-first workflow (epic #205)

- `webfront::WebFront app; app.show();` is a complete program (`src/HelloWorld.cpp`). With no document root WebFront serves an embedded "Hello, WebFront!" page whose status badge shows **connected** only after `webFront.ready` resolves (WebSocket handshake acknowledgement) and an error state if the initial connection fails. `ready` does not track later disconnects.
- `WebFrontGreeter` (`src/Greeter.cpp`, `src/greeter/`) uses an explicit development document root and a deduced `greet(std::string)` callback; the page awaits the returned Promise and shows success or errors via `textContent`.
- The option-free `cmake ..` / `cmake --build .` build downloads neither CEF nor Catch2 (`ENABLE_TESTING` and `WEBFRONT_EMBED_CEF` default to `OFF`) and builds the examples into `<build>/src`. As a subproject only `WebFront::WebFront` is added: no examples, tests, warning flags, or global compiler settings leak into the parent.
- The CTest cases `NewcomerDocsMatchExamples` and `AddSubdirectoryConsumer` guard the documented commands, snippets and subproject integration.
- Verified 2026-10-13 on Linux (Ubuntu 24.04, GCC 13.3, CMake 4.4.4): option-free build; CEF-off testing build with all CTest cases passing; Hello and Greeter pages served and driven in a real Chromium (connected badge, empty-input message, `Hello, Ada!`). **Not verified in that environment:** the CEF-enabled build and the Jasmine/Xvfb integration (the CEF download was blocked), Windows, macOS and Clang.
- Limitation: a configured document root serves arbitrary local files and is a development convenience, not a production sandbox or packaging format.

## UI components: the terminal and the tool manager

- First opt-in UI component (#230), designed in [UI_COMPONENTS.md](UI_COMPONENTS.md): `webfront::Terminal` (`include/components/Terminal.hpp`) registers C++ commands behind an embedded xterm.js 6.0.0 terminal (`fs::TerminalFS`, served under `webfront/components/terminal/`; composed with `fs::DefaultFSWith<>`). Example: `WebFrontTerminal` (`src/Terminal.cpp`). The bridge protocol is unchanged.
- Line editing, history and Tab completion run in the browser; C++ receives whole lines and returns the output as the Promise result. A command exception rejects the Promise and is printed as an error. Output is capped at 60000 bytes because bridge strings carry a 16-bit length.
- Covered by Catch2 (`TerminalTests.cpp`, `TerminalFSTests.cpp`), five asset freshness CTest cases and Jasmine specs (`webtest/js/terminal-specs.mjs`) in the browser integration test.
- Verified 2026-10-14 on Linux (GCC 16.1, Unix Makefiles generator): CEF-off build with all 100 CTest cases passing; the example driven in headless Chromium (commands, errors, quoting, completion, history, `help`, `clear`); and the Jasmine suite, terminal specs included, run in headless Chromium through a scratch copy of the runner that replaces `CEFFrontend`. A deliberately broken terminal spec made that run fail. **Not verified:** the CEF/Xvfb CTest itself, Windows, macOS and Clang.
- Limitations: no output streaming, no cancellation, no per-browser sessions, and single-row line redraw (see the design note).
- The `wf` tool manager (#234, npm-hosted web tools installed during debugging): `webfront::ToolManager` (`include/components/ToolManager.hpp`) stores tools with `fs::ToolStore` under `.webfront/tools/`, records them in `webfront-tools.lock.json`, serves them with `fs::PackageFS` under `webfront/tools/` and runs `wf list`, `wf remove` and `wf help` in C++. `wf search`, `wf install` and `wf open` run in the page, in `wf.mjs` (`src/components/tools/`, served by `fs::ToolManagerFS`), a terminal extension module loaded through the new `Terminal::extension()`: it fetches the packument and tarball, checks the SHA-512 `dist.integrity` with `crypto.subtle`, gunzips with `DecompressionStream`, reads the ustar archive (pax and prefixed paths included) and sends the files in 256 KiB chunks through `webfront.pkg.*`, after refusing archives unpacking to more than 256 MiB. Ctrl+C interrupts it through the `signal` the terminal passes to browser commands. The registry is set by `ToolManagerOptions::registry`. Example: `WebFrontTools` (`src/Tools.cpp`). Covered by `ToolStoreTests.cpp`, `ToolManagerTests.cpp`, `TerminalTests.cpp` and the Jasmine specs `webtest/js/tools-specs.mjs`, which install a fixture package from a fake registry served by the test (`webtest/registry/`, regenerated by `make_fixture.py`) into a temporary directory.
- Verified 2026-10-15 on macOS (Apple clang, CEF off): all CTest cases pass except `AddSubdirectoryConsumer`, which fails locally because of the outdated libc++ headers of the Command Line Tools; the Jasmine suite, tools specs included, run in Chromium through a scratch copy of the runner that replaces `CEFFrontend` (one terminal spec, "reports an application without terminal functions", fails there as it does on develop, because that browser gave its unsized terminal a two-column width); `WebFrontTools` driven against registry.npmjs.org (`wf help`, `wf search`, `wf list`, `wf install left-pad` downloaded, verified and refused as not a tool). **Not verified locally:** the CEF/Xvfb CTest (Linux only; the macOS CEF build of `webtest` fails inside stdexec), Windows.
- Limitations of the tool manager: no semver ranges (an exact version or a dist-tag), no dependencies (a tool must bundle what it uses), no progress bar beyond step messages, sequential chunk uploads, and the trust model of `npm install`.
- Found while building it: the C++ encoder wrapped the 16-bit length of strings over 65535 bytes (#231, fixed: such strings now fail explicitly, see "Implemented baseline"); `MimeType::toString()` mapped `csv` to `text/css` (#232, fixed: a `static_assert` now ties the name table to the enum); with GCC 16.1 and the Ninja generator, CMake's module dependency scanning (`-fmodules-ts`) crashes the compiler inside stdexec, while Unix Makefiles builds work.

## Intended product

WebFront is a C++23 header-only experiment for browser-based C++ user interfaces. A C++ process serves local HTML/JavaScript and exchanges typed function-call messages with the page over WebSocket. It supports the system browser, embedded CEF, and an optional Windows WebView2 frontend.

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
- `WEBFRONT_EMBED_CEF=ON` uses CEF 154.0.34 (Chromium 154.0.8037.98, package `ambroise-leclerc/CEF@154.0.34`). An existing CEF 137 build directory must be reconfigured from scratch. On Windows, `libEGL.dll`, `libGLESv2.dll`, `dxcompiler.dll`, `dxil.dll` and `vk_swiftshader_icd.json` are copied only when the distribution ships them (CEF 154 has no ANGLE DLLs, #249). Verified 2026-10-16 on Linux (GCC 16.1, Unix Makefiles): the CEF build with all 138 CTest cases passing, `WebFrontBrowserIntegration` included, and the CEF-off build with all 137; with Clang 22, the CEF build of `webtest` and `WebFrontBrowserIntegration`. **Not verified:** CEF 154 on Windows and macOS (the macOS ARM64 framework requires macOS 13.0+). A parallel build of every example fails intermittently because their post-build steps copy `locales/` into the same `src/` directory at once.
- The bridge carries all JavaScript numeric typed-array variants in both directions. C++ `std::vector`, `std::array`, and `std::span` values use the matching typed-array encoding, while decoded parameters own their storage.
- `src/WebFront.js` is the readable browser bridge source. CMake deterministically regenerates the embedded `include/system/WebFrontJsData.hpp` asset and CTest rejects a stale generated copy.
- Text assets are embedded **verbatim rather than compressed**, so `HTTPServer` serves them without a `Content-Encoding` header. Deflate streams are not reproducible across the zlib builds on our macOS, Linux, and Windows runners, so a pre-compressed blob could not be verified by the staleness check. Restoring compression — either a reproducible build-time gzip or on-the-fly compression keyed on `Accept-Encoding` — is tracked as a follow-up. The fallback HTML and module have readable sources under `src/fallback/`; `favicon.ico` remains Brotli-encoded. Regenerate the text headers with `cmake --build build --target webfront-embedded-assets`. Sources and generated headers use LF on every platform so the CTest freshness checks compare identical bytes.
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
  This is a development frontend for trusted content: navigation and popup requests are unrestricted, DevTools and context menus
  remain enabled, and the adapter supplies no origin-based bridge authorization. Windows/Shell/WRL header names leak into consumers;
  qualify `std::byte`. Implementation types live under `frontend::detail::webview2`. The runtime-creation timeout defaults to 30 seconds,
  is configurable through `WebView2Options`, and stops before page navigation. Profile directories are also configurable.
  All built-in frontends receive the configured address and preserve URI query/fragment syntax and existing percent escapes.
  Controller closure is idempotent and reports HRESULT failures; DPI-change messages apply the suggested window bounds.
  The Windows CI matrix runs the shared Jasmine bridge suite and timed lifecycle tests under WebView2; CEF/Xvfb remains the Linux lane.
  The WebView2 browser label runs separately with `--no-tests=error`. CI's Evergreen bootstrapper URL is mutable;
  a downloaded installer must carry a valid Microsoft signature, and installed executable versions are logged.
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
  CEF/Xvfb was not run on this Windows host; the available WSL launcher fails during its own initialization.

Local verification on 2026-07-19 completed successfully: the CEF-off build passed all 47 CTest cases, including deterministic asset and typed-array protocol coverage, and a CEF-enabled build passed `WebFrontBrowserIntegration` under Xvfb. The browser test exercised every supported numeric typed array from C++ to JavaScript and JavaScript to C++, ordinary tuple arrays, final Jasmine reporting, and automatic CEF shutdown.

## Known limitations and next decisions

- Result values are currently limited to the bridge's existing scalar, string, tuple, and numeric typed-array parameter types; richer object/DOM values remain out of scope.
- The public API does not yet expose cancellation for an outstanding future or Promise.
- Ordinary JavaScript arrays encode as tuple-like parameters; only numeric typed arrays use the homogeneous array wire representation.
- `NativeDebugFS` exposes development files and is not a production packaging format.
- CEF is a large optional dependency. Only the focused Linux integration job should download it for the minimal baseline.
- React/Babel assets remain examples rather than the core demonstration. Modern Node/Vite integration, overlays, and packaged filesystems require separate product decisions. WebView2 fixed-version runtime packaging remains out of scope.
