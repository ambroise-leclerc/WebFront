# WebFront project status

Status snapshot: 2026-07-16, branch `132-javascript-modules`, based on `develop` commit `4eac25f`.

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
- `AGENTS.md` is the canonical development guide for supported coding agents.

Local verification on 2026-07-16 completed successfully: the CEF-off build passed all 44 Catch2 scenarios, JavaScript module syntax checks passed, coverage flags appeared when `ENABLE_COVERAGE=ON`, and a fresh CEF-enabled build passed `WebFrontBrowserIntegration` under Xvfb. The verbose browser run showed both tokenized bridge calls, a `passed` Jasmine report, and automatic CEF shutdown.

## Known limitations and next decisions

- JavaScript/C++ function results and remote exceptions are not propagated. The existing `FunctionReturn` message type is protocol groundwork, not a working public feature.
- Bridge lookup and connection errors need a defined cross-language error contract.
- Arrays/typed arrays are incomplete; JavaScript arrays currently encode as tuple-like parameters.
- `NativeDebugFS` exposes development files and is not a production packaging format.
- CEF is a large optional dependency. Only the focused Linux integration job should download it for the minimal baseline.
- React/Babel assets remain examples rather than the core demonstration. Modern Node/Vite integration, WebView2, overlays, and packaged filesystems require separate product decisions.
