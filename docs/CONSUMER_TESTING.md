# Testing an existing WebFront application with Jasmine and CTest

`webfront_add_jasmine_test` instruments the application's actual page. Its document root,
ordinary scripts and C++ callbacks remain available to the specs. It uses the real Jasmine,
without the developer terminal, a wf manifest or a JavaScript runtime on the host.

An application must explicitly enable instrumentation in a test build. Before opening the page,
retain `webfront::testing::JasmineTest`; after the window closes, shut down and return its status:

```cpp
#ifdef WEBFRONT_JASMINE_TESTING
#include <webfront/testing/Jasmine.hpp>
#endif

// Construct app with an embedded frontend and port "0" to allow parallel tests.
// Register the application's ordinary callbacks before show().
#ifdef WEBFRONT_JASMINE_TESTING
webfront::testing::JasmineTest tests{app};
#endif
app.show();
app.shutdown();
#ifdef WEBFRONT_JASMINE_TESTING
return tests.exitCode();
#else
return 0;
#endif
```

In the consuming CMake project, after `enable_testing()`, `add_subdirectory(WebFront)` and
creating the application target:

```cmake
webfront_add_jasmine_test(NAME BoardUI TARGET SiliconForge
    SPECS tests/ui/board.spec.js ARGS firmware.bin FRONTEND WKWebView TIMEOUT 60)
```

The helper is available after adding WebFront; registration defines `WEBFRONT_JASMINE_TESTING`
on the specified target. Build the application and run `ctest --output-on-failure`. Without that
macro `JasmineTest` is an inert class that registers nothing and never reads
`WEBFRONT_JASMINE_CONFIG`, so a distribution build cannot be turned into a script loader through
the environment, even if it constructs the object unconditionally. Define the macro consistently
for every translation unit of one program.

`FRONTEND` validates that the named embedded engine is enabled for the current platform; the
application still selects its C++ frontend explicitly. CEF and WebKitGTK work headlessly under
Linux's `xvfb-run`, which must be installed. WKWebView and WebView2 require their native desktop
session. The system browser is unsuitable for these automated tests. If using the test driver's
frontend wrappers, they consume the isolated profile paths supplied by the runner. Other
applications must configure their frontend's persistent profile explicitly if it has one.

`ARGS` are passed literally as individual arguments, without a shell: empty strings, semicolons
and `$<` reach the application unchanged; an argument containing `]==]` is refused.
`TIMEOUT` defaults to 60 seconds (1–3600). The runner enforces it outside the application; CTest
has a slightly longer deadline. Therefore a synchronous infinite loop in a spec cannot report
success or hang CI indefinitely. On POSIX hosts the runner starts the application in its own
process group and kills the whole group at the deadline, so a renderer or `xvfb-run`'s X server
does not outlive it. On Windows only the direct child is stopped; check for a leftover renderer
after a timeout. Ports are not changed by the helper: use an OS-assigned port in the application.

`SPECS` are existing classic JavaScript files, loaded in order after the real page's load event
and bridge handshake. They may await application-specific readiness in `beforeAll`. Compile
TypeScript or module graphs into classic JavaScript beforehand; relative ES module imports and
TS compilation are not provided by this helper. Individual files are limited to 4 MiB, with at
most 100 spec files. A syntax error or top-level exception in any of them fails the run with a
`status: error` report naming the file; the other specs are not run on their own. The application needs its ordinary `/WebFront.js` and an embedded window
that permits the instrumentation script and closes on `window.close()`.

By default configure downloads and SHA-512 verifies `jasmine-core@7.0.2`, then extracts only
its browser runtime and license into the build directory. To configure offline, set
`WEBFRONT_JASMINE_ARCHIVE_DIR` to a directory holding the exact pinned
`jasmine-core-7.0.2.tgz`. `JASMINE /path/to/jasmine.js` instead uses a caller-provisioned runtime;
its provenance and version are the caller's responsibility. No public network is needed at test
execution. Jasmine stays outside the core's embedded resource list.

Each named test owns `webfront-jasmine/<name>/` in the calling binary directory: config, profile
and bounded JSON report. Names contain only letters, digits, underscore and hyphen. Do not run
two independent CTest processes for the same named test/build simultaneously. Use separate build
trees for those runs. A failed spec, suite/global error, focused suite, zero executed specs,
missing report, application failure or timeout fails CTest. Old reports are removed before each run.

The test build trusts the configured runtime/specs and application page. This is code execution
with application/bridge authority, not a sandbox. Native reads expose only the configured files
by numeric index, in bounded chunks, to the newest instrumented connection: a page that reloads
or redirects is instrumented again and the earlier connection loses access. One window is supported. No runtime path supplied
by a page is opened. Use an isolated CI environment for untrusted projects and remove secrets.

## Screenshots and image sequences

A test build can capture the **web content** of the window (without native decorations) as PNG images, for example to
build a README animation in CI. Each named test owns `webfront-jasmine/<name>/captures/`, emptied before every run. The
specs reach it through the global `webFrontCapture`:

```js
it('shows the board running', async () => {
    await webFrontCapture.screenshot('start');                       // captures/start.png
    const frames = await webFrontCapture.record('run', {seconds: 4, framesPerSecond: 12});
    // captures/run/0001.png ... 0048.png; frames is the number written
});
// Or bracket a scenario yourself: startRecording(name, framesPerSecond) ... stopRecording() -> frames
```

Encode the sequence outside WebFront: `ffmpeg -framerate 12 -i captures/run/%04d.png run.webm` (or gifski for a GIF).
Names have 1 to 64 letters, digits, `_` or `-`, so a page can neither leave the directory nor pick a path; rates go from
1 to 30 images per second and a recording stops at 1800 images. Only one recording runs at a time, on its own thread,
so the bridge and the page keep running while it records. A screenshot or the end of a recording does not block the
server thread either: the bridge callbacks only start the work on a worker thread and the page polls for its end (every
20 ms) before the Promise settles. Capturing is paced by the engine: a slow capture drops
instants rather than bursting, so the number of images can be below `seconds * framesPerSecond`. Stopping a recording
whose window disappeared throws the capture error. A recording that ends by itself (an error, or the 1800 images limit)
keeps its slot until `stopRecording()` collects it, so the next `startRecording` needs that call first.

From C++, `webfront::frontend::capturePng(timeout)` returns the PNG bytes (or a diagnostic) of the open window from any
thread other than the one running its event loop, and `webfront::testing::CaptureStore` writes them to a directory
(`screenshot`/`stopRecording` wait; `beginScreenshot`/`pollScreenshot` and `requestStop`/`pollRecording` do not).
One window per process is supported.

| Frontend | Capture |
| --- | --- |
| WebKitGTK | Supported (`webkit_web_view_get_snapshot`, GTK 4.6 or newer, otherwise the call fails with a diagnostic), validated under `xvfb-run`; the sandbox opt-out variable is not needed by the capture itself. |
| WKWebView | Supported (`takeSnapshotWithConfiguration:`, encoded as PNG by AppKit); the request runs on the main thread, which must be running the window's event loop. |
| WebView2, CEF | Not implemented yet: the call fails with "no window that can be captured is open". |

This is an opt-in test facility with the same trust model as the rest of this guide: it is compiled only with
`WEBFRONT_JASMINE_TESTING`, and the captured page can contain secrets it displays. Automation/MCP does not expose
captures yet.
