# Frontends

[Back to the README](../README.md#use-webfront-in-your-project)

A frontend decides how the interface is displayed: the system browser (`DefaultBrowserFrontend`, the default), an embedded Chromium window (`CEFFrontend`) or an embedded native web view (`WebView2Frontend` on Windows, `WKWebViewFrontend` on macOS, `WebKitGTKFrontend` on Linux). The application code and interface assets do not change when the frontend does. Platform-specific notes live next to each frontend, for example [WebView2](WEBVIEW2.md) and [WKWebView](WKWEBVIEW.md). This page states the rules they all follow; the C++ concept is `frontend::FrontendType` in `include/webfront/frontend/Frontend.hpp`.

## The contract

A frontend is a type with:

| Member | Meaning |
| --- | --- |
| `static void initialize()` | Called once successfully per frontend type, before the first `BasicWF` listener is created. A thrown exception aborts construction; the next construction retries. |
| `static void open(std::string_view port, std::string_view file)` | Called by `show()` / `openAndRun()` on the calling thread, while the HTTP server runs on its own thread. |
| `static void open(address, port, page, const ContentPolicy&)` (optional) | Preferred overload receiving the per-application [content policy](CONTENT_POLICY.md). |
| `static constexpr Action action` | What `BasicWF` does once `open()` returns. |

- **Blocking.** With `Action::closeServerAfterOpen` (embedded windows), `open()` blocks until the window is closed, releases its session resources, and returns; `BasicWF` then stops the server and joins its thread, so `show()` returns after a clean shutdown. With `Action::keepServerRunning` (the system browser), the server keeps running after `open()` returns, until `stop()` or application destruction. The action determines server lifetime, not whether opening returns immediately: the built-in system-browser adapter waits for Enter in the console after launching the page and cannot observe when the external browser closes.
- **Threading.** Construct the application and call `show()` from its main thread when selecting an embedded frontend, because initialization can also have thread affinity. `show()` runs `open()` on that calling thread while the server runs separately. AppKit and GTK require toolkit-specific thread checks, and WebView2 needs an STA-compatible thread (an MTA thread is rejected with a clear error). Thread validation belongs in each adapter: the C++ concept checks signatures and supplies no main-thread detection or automatic dispatch. No `requiresMainThread` flag is currently needed; the native adapters must check their toolkit's requirements before using it.
- **Errors.** A frontend reports failure by throwing from `open()`, whatever its action. `show()` / `openAndRun()` stop and join the server before the original exception propagates, including non-standard exceptions, so a failed start leaves no running thread behind. `openWindow()` is a lower-level delegation API: it neither starts nor joins the server, and returns the requested `WindowAction` to its caller.
- **Unavailable frontends.** Every frontend type is always declared. In a build without its support, initialization or opening throws a message naming the CMake option that enables it (`-DWEBFRONT_EMBED_CEF=ON`, `-DWEBFRONT_EMBED_WEBVIEW2=ON`, `-DWEBFRONT_EMBED_WKWEBVIEW=ON`, `-DWEBFRONT_EMBED_WEBKITGTK=ON`).
- **URL.** Built-in frontends also provide `open(address, port, file)`, which `BasicWF` prefers when available. Build the URL with `frontend::detail::applicationUrl(address, port, file)` in `frontend/Url.hpp`: it uses the configured address and preserves URI query/fragment syntax and existing percent escapes. Spaces and UTF-8 bytes are encoded. For literal filename delimiters use `%23`, `%3F` and `%25`. The two-argument overload uses `127.0.0.1`; wildcard binds open their matching loopback address.
- **Selection.** `webfront::WebFront` always uses the system browser. An application opts into another frontend with `webfront::WebFrontWithFrontend<...>`; enabling an embedded engine at configuration time never changes the default.

## Window title and size

`WebFrontConfig::window` (`webfront::WindowOptions`: `title`, `width`, `height`) sets the initial window of the
embedded frontends (WebKitGTK, CEF, WebView2, WKWebView); the system browser ignores it. An empty title selects the
executable's name ("WebFront" if it cannot be determined), and a non-positive size selects 1200 x 800. `BasicWF` passes the
options to a frontend that provides `open(address, port, file, policy, window)`; WebView2 and WKWebView also accept
them in `WebView2Options::window` and `WKWebViewOptions::window`. The title is fixed at creation: following
`document.title` is not implemented.

## Content policy

The [shared content policy](CONTENT_POLICY.md) defines bridge-origin authorization, embedded navigation, popup handling, DevTools and context-menu settings, including defaults and opt-outs. The system browser cannot expose embedded navigation/UI hooks; its bridge is protected by the same server check.

## Contract tests

`test/unit/FrontendContractTests.cpp` uses a synchronized mock window and a server loop that stays active
until stopped. It verifies blocking until closure, execution on the calling thread, server-loop
completion before `show()` returns or propagates an exception, and continued service after a
`keepServerRunning` frontend returns. Standard and non-standard exceptions are covered, including
startup failures before any window is created. Coordination waits have a 30-second upper bound.
`test/unit/WebFrontTests.cpp` also checks initialization once per frontend type, retry after failed
initialization, frontend selection and unavailable-engine diagnostics. These tests need no GUI.

## Adding a frontend

1. Implement the type in `include/webfront/frontend/` following the contract above and include it from `Frontend.hpp`. Keep platform headers behind the option's macro.
2. Add its support module and one `webfront_register_frontend(...)` entry in `cmake/Frontends.cmake`, using the fields below.
3. Extend `FrontendSelection` coverage and run the shared Jasmine and lifecycle suites. Enabling the engine on a `TEST_PLATFORMS` platform automatically registers the seven shared lifecycle cases; `WEBFRONT_TEST_FRONTEND` selects its C++ type in both runners.

| Field | Meaning |
| --- | --- |
| `OPTION` | Availability option named `WEBFRONT_EMBED_<NAME>`, always defaulting to `OFF`. |
| `DESCRIPTION` | Plain-text CMake option help, checked against the option table below after removing Markdown code delimiters. |
| `PLATFORMS` | Supported CMake platform names. Enabling the option elsewhere fails before fetching dependencies. |
| `SUPPORT` | Module loaded when the engine is enabled. Registration checks that its file exists. |
| `SETUP` | Optional function configuring each application's target, including runtime deployment. |
| `POLICY` | `strict` rejects an unavailable engine for the browser runner; `lenient` permits a diagnostic build. CEF uses `lenient` to retain its CEF-off runner. |
| `TEST_PLATFORMS` | Platforms with a validated browser runner, independently of engine availability. Currently Linux for CEF and WebKitGTK, Windows for WebView2 and macOS for WKWebView. |
| `TEST_NAME`, `TEST_TIMEOUT` | Shared Jasmine CTest name and positive timeout in seconds. |
| `TEST_SETUP` | Optional function adding engine-specific tests. It receives the explicit list of registered shared tests (including Jasmine), and runs even if missing Xvfb prevents shared registration; the module decides which additional tests need a display. |
| `XVFB_LINUX` | Wrap the shared tests with Xvfb on Linux. Other registered test platforms run without that wrapper. |
| `ALWAYS_AVAILABLE` | Declare a frontend without an availability option. Defaults to `POLICY lenient`, rejects `POLICY strict`, and cannot supply engine or test fields. |

The selectors, cache choices, diagnostics, dependency loading and CTest registration use this entry. The optional `SETUP` and `TEST_SETUP` functions are checked immediately after the enabled support module is loaded.

Registry order determines the initial cache default: the first enabled embedded engine supported on the target platform wins. Reconfiguration preserves that cached choice. WebView2 precedes CEF on Windows, WKWebView precedes CEF on macOS. Use `-DWEBFRONT_WEBTEST_FRONTEND=WebView2` to switch explicitly, or `-UWEBFRONT_WEBTEST_FRONTEND` to recompute the default. Selecting CEF on Windows or macOS emits a warning because no browser test is validated there. With no embedded engine enabled, the runner retains CEF for its diagnostic build and registers no browser test.

## Lifecycle tests

`test/web/FrontendLifecycle.cpp` runs the same seven cases under the selected embedded engine:

- `no-bridge`: close a page that never connects to the bridge and return from `show()`.
- `early-close`: close immediately from the page's first script, before any bridge connection.
- `rejected-ready`: reject `webFront.ready` using an unreachable WebSocket endpoint, handle the rejection and close.
- `navigation-failure`: open an unreachable initial URL and require a navigation exception.
- `path-encoding`: load `encoded path #.html` and verify that its query and fragment survived.
- `content-policy`: block a popup and a foreign-origin top-level navigation while keeping the original page alive.
- `navigation-allow-list`: permit the same top-level destination through the explicit navigation allow-list.

Each runs in a separate process as `<Frontend>Lifecycle-<case>`, with the registry's timeout, the
`web-integration` label and the same port lock as Jasmine. Linux CEF and WebKitGTK cases use Xvfb. Build `webtest`
to build both shared runners on a validated test platform, or build all targets to include engine-specific runners as well. Run
`ctest --test-dir <build> -L web-integration --output-on-failure` for the complete browser suite.
A missing Xvfb skips both shared suites. A new frontend inherits this coverage through its registry
entry; no engine-specific lifecycle runner is required.

`CEFContentPolicy` additionally opens DevTools under Xvfb and checks the runtime command IDs and context-menu filtering.

WebView2 retains a separate runner for COM, missing runtimes, startup timers, later navigation
failures, slow page loading, repeated `show()` calls and native closure during window creation.
Repeated-show coverage includes rejected ready and encoded paths; a separate navigation case also
checks the WebView2 error diagnostic. These cases use WebView2's implementation details and keep
their own CTest timeouts.

WKWebView adds native main-thread, Quit/closure, repeated-opening, initial HTTP failure, stalled-loading
deadline and data-store/inspector/menu tests. See [WKWebView verification](WKWEBVIEW.md#unattended-verification).

## Direct engine access

Every frontend currently loads its page over HTTP and exchanges bridge messages over a WebSocket. [ADR-017](adr/ADR-017-acces-direct-aux-frontends.md)
(proposed) describes how an adapter may later add its engine's native resource scheme and message channel without replacing that
path. Transport benchmarks are **manual measurement tools**, excluded from the default build,
CTest and every CI workflow. The targets are `webkitgtk_transport_benchmark`,
`webview2_transport_benchmark` and `wkwebview_transport_benchmark` for their respective engines.
Build one explicitly, then launch its executable (the macOS executable is inside its `.app` bundle).

For comparable Linux timings, use Release without coverage:

```bash
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=ON \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK
cmake --build build-performance --target webkitgtk_transport_benchmark --parallel 2
cd test/web
xvfb-run -a ../../build-performance/test/web/webkitgtk_transport_benchmark /tmp/webkitgtk-transport-benchmark.json
```

The former `performance-linux` CI job and timed transport CTest cases have been removed (#283).
The shared page still uses the same sizes, call counts, warm-up, five samples and concurrent
calls when launched manually. `WebKitGTKTransportContracts` and `WebView2TransportContracts`
exercise native string replies, C++ evaluation, wrong echoes and evaluation exceptions with
at most two calls per size; WebView2 also checks shared-buffer calls in both directions.
They use a separate page without timing batches or performance reports.
`WKWebViewTransportContracts` runs a separate contracts-only
executable in CTest; it does not execute measurement batches.

Each prints a table, writes `webkitgtk-transport-benchmark.json`, `webview2-transport-benchmark.json` or
`wkwebview-transport-benchmark.json` in the build directory
of `test/web`, and fails only on a wrong echo. WebView2 also measures a shared buffer, which is the only WebView2 direct path
that beats the HTML bridge at every size; its string messages do not. The ADR gives the figures and the recommended default
per platform. WKWebView measures `evaluateJavaScript`, `callAsyncJavaScript` and a base64 byte envelope; its public API
documents Foundation values rather than a shared binary buffer. `WKWebViewTransportContracts` checks replies, errors,
Promises, incorrect echoes, base64 bytes and subframe rejection separately from timings. These test-only native channels
do not change the production HTTP/WebSocket transport.

## Build options

Enabling an engine makes its adapter available; select it explicitly in your application.
`webfront::NativeApp` chooses the native engine for the current platform.

The options default to `OFF`. Configuration first probes the native engine of the platform (registered with `NATIVE`: WebKitGTK through
`pkg-config webkitgtk-6.0`, WKWebView on Clang with macOS 13.3+, WebView2 with an MSVC-compatible compiler) and sets `ON` or `OFF` in a normal variable, so `cmake ..` is enough to build the Life
example with a native window. Automatic values do not create engine cache entries and are probed again at the next configuration.
An explicit normal or cached option is never replaced, even if it equals the last detected value. Remove the cache entry with
`-UWEBFRONT_EMBED_WKWEBVIEW` (or the corresponding platform option) to resume automatic detection.
A missing engine only prints a status; the Life examples then use the default browser and CMake explains what to install.
Platforms without a registered native engine use the default browser as well. `-DWEBFRONT_AUTODETECT_NATIVE=OFF`
turns the detection off. CEF is never detected.
`ENABLE_TESTING` and `ENABLE_FUZZING` only disable the detection of a top-level WebFront build: a
parent project that defines them for its own purposes does not change the detection of its consumers.

Consumers can call `webfront_use_native_window(target)` after linking `WebFront::WebFront`.
It applies the native engine's setup and defines `WEBFRONT_NATIVE_WINDOW` on that target when
enabled. Application code uses that definition to select `NativeWebViewFrontend` or the
default browser, as shown in the [README](../README.md#use-webfront-in-your-project).
The registry and configured options are shared across directories: the lower-level
`webfront_native_frontend_option(out)` and `webfront_setup_frontends_for_target(target)`
also work from a parent or sibling directory. The latter sets up every enabled engine;
the native helper never selects CEF.

| Option | Default | Description |
| --- | --- | --- |
| `WEBFRONT_EMBED_WEBVIEW2` | OFF | Download the pinned WebView2 SDK and link its static loader (Windows only) |
| `WEBFRONT_EMBED_WKWEBVIEW` | OFF | Link the system Cocoa and WebKit frameworks for WKWebViewFrontend (macOS 13.3+ only) |
| `WEBFRONT_EMBED_WEBKITGTK` | OFF | Enable the system WebKitGTK 6.0 embedded frontend (Linux only) |
| `WEBFRONT_EMBED_CEF` | OFF | Download and link CEF so CEFFrontend is available (Windows, Linux and macOS only). Other platforms fail at configure time |
