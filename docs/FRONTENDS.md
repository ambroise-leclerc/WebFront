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
| `static void open(address, port, page, const ContentPolicy&, const WindowOptions&, const ViewLaunch&)` (optional) | Richest overload: also receives the [view association](#view-association) proof of this launch. |
| `static constexpr Action action` | What `BasicWF` does once `open()` returns. |

- **Blocking.** With `Action::closeServerAfterOpen` (embedded windows), `open()` blocks until the window is closed, releases its session resources, and returns; `BasicWF` then stops the server and joins its thread, so `show()` returns after a clean shutdown. With `Action::keepServerRunning` (the system browser), the server keeps running after `open()` returns, until `stop()` or application destruction. The action determines server lifetime, not whether opening returns immediately: the built-in system-browser adapter waits for Enter in the console after launching the page and cannot observe when the external browser closes.
- **Threading.** Construct the application and call `show()` from its main thread when selecting an embedded frontend, because initialization can also have thread affinity. `show()` runs `open()` on that calling thread while the server runs separately. AppKit and GTK require toolkit-specific thread checks, and WebView2 needs an STA-compatible thread (an MTA thread is rejected with a clear error). Thread validation belongs in each adapter: the C++ concept checks signatures and supplies no main-thread detection or automatic dispatch. No `requiresMainThread` flag is currently needed; the native adapters must check their toolkit's requirements before using it.
- **Errors.** A frontend reports failure by throwing from `open()`, whatever its action. `show()` / `openAndRun()` stop and join the server before the original exception propagates, including non-standard exceptions, so a failed start leaves no running thread behind. `openWindow()` is a lower-level delegation API: it neither starts nor joins the server, and returns the requested `WindowAction` to its caller.
- **Unavailable frontends.** Every frontend type is always declared. In a build without its support, initialization or opening throws a message naming the CMake option that enables it (`-DWEBFRONT_EMBED_CEF=ON`, `-DWEBFRONT_EMBED_WEBVIEW2=ON`, `-DWEBFRONT_EMBED_WKWEBVIEW=ON`, `-DWEBFRONT_EMBED_WEBKITGTK=ON`).
- **URL.** Built-in frontends also provide `open(address, port, file)`, which `BasicWF` prefers when available. Build the URL with `frontend::detail::applicationUrl(address, port, file)` in `frontend/Url.hpp`: it uses the configured address and preserves URI query/fragment syntax and existing percent escapes. Spaces and UTF-8 bytes are encoded. For literal filename delimiters use `%23`, `%3F` and `%25`. The two-argument overload uses `127.0.0.1`; wildcard binds open their matching loopback address.
- **Selection.** `webfront::WebFront` always uses the system browser. An application opts into another frontend with `webfront::WebFrontWithFrontend<...>`; enabling an embedded engine at configuration time never changes the default.

## View association

`UI::isEmbeddedView()` tells whether a connection comes from the view of an embedded frontend of this application, and
`UI::isLocalClient()` is the only criterion of a "local client" for system services (ADR-020, rule 3). A loopback address
proves nothing: a remote browser behind a local proxy or an SSH tunnel, or another user of the machine, arrives from it too.

Each launch creates a random 256-bit secret. A frontend that implements the `open(..., const ViewLaunch&)` overload gives it
to its own view, in the process, at the start of every top-level document whose origin is the application's
(`ViewLaunch::origin`). The page receives it as the window property `__webfrontView`; `WebFront.js` takes it when it loads, removes
it from the window and presents it in the handshake of every connection of that page (a reconnection included). The server
accepts one handshake per connection and compares the secret in constant time; a proof of another size than 64 characters is
not even copied. It is never served over HTTP and never put
on a command line or in a URL.

| Engine | Injection at the start of the document |
| --- | --- |
| WKWebView | `WKUserScript`, `WKUserScriptInjectionTimeAtDocumentStart`, main frame only |
| WebView2 | `AddScriptToExecuteOnDocumentCreated`, top-level documents of the origin (the script checks `window === window.top`); the first navigation waits for successful registration in its completion handler, with the startup watchdog active during that wait; registration failure fails the opening |
| WebKitGTK | `WebKitUserScript` at document start, top frame, no URL allow list (the script compares `location.origin`, port included) |
| CEF | extra browser information given to `CefBrowserView::CreateBrowserView`, read by the renderer process's `OnBrowserCreated` and defined on the window by `OnContextCreated` through the same script as the other engines, evaluated in the new context so that the page itself answers for its origin (the render process app is the one given to `CefExecuteProcess`; a separate helper executable must give it its own `ViewSecretRenderApp`) |

- The system browser receives no secret (a command line or a history could disclose it, and a remote browser would present
  it first): its connections are never local, whatever their address. An application that accepts that risk sets
  `WebFrontConfig::trustLoopbackClients`, which reads the peer address and is unsafe on a shared machine or behind a proxy or tunnel.
- `NativeOrBrowserFrontend` passes the proof to the native window only.
- The association does not protect against a script admitted into the view (ADR-008): it shares the authority of the bridge
  and could read the property before `WebFront.js`. An adapter that cannot inject a script simply does not implement the overload.
- A custom frontend that wants services restricted to its own view implements the overload and applies
  `webfront::detail::viewSecretScript(secret, origin)` (or the engine's equivalent) at the start of the document. The helper
  returns an empty script, hence no association, unless the secret is lowercase hexadecimal and the origin a plain http origin;
  give it `webfront::detail::browserOrigin(address, port)` (lowercase, without port 80), which `ViewLaunch::origin` already is.
  The script uses the browser's `URL` parser to serialize that origin before comparing it with `location.origin`, including
  IPv6 compression and embedded IPv4 notation (`::ffff:127.0.0.1` becomes `[::ffff:7f00:1]` as a URL host).

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

## File chooser and downloads

The embedded frontends use their engine's file chooser and download handling without a WebFront API (ADR-020, rule 6).
Coverage varies by engine and does not yet qualify all native dialogs, `accept` filters or directory selection. A `blob:` URL of an
admitted origin passes the navigation policy (`webfront::detail::blobOriginUrl`); a `blob:` URL of any other origin does not.

| Frontend | Chooser | Download | Test injection |
| --- | --- | --- | --- |
| WebKitGTK | default `run-file-chooser` dialog (portal under Flatpak) | `download-started` / `decide-destination`: the user's XDG download folder (`~/Downloads` when none is configured, never the home folder itself; leading dots are removed from names), the page's name reduced to its last component, never overwriting (`name (1).ext`), also between downloads that run at the same time (`DownloadReservations`) | `Session::chooseFiles`, `downloadDirectory`, `downloadFinished` |
| CEF (Chrome runtime) | the runtime's dialog | the runtime's download handling | `SimpleCEFClient::chooseFiles` (`CefDialogHandler::OnFileDialog`), `downloadDirectory`, `downloadFinished`. The runtime itself turns a suggested name `../x` into `_x` |
| WKWebView | `runOpenPanelWithParameters` shows an `NSOpenPanel` sheet of the window; only frames of the application's own origin | `WKDownload` (macOS 11.3+): a save panel proposes the name | `WKWebViewOptions::chooseFiles`, `downloadDirectory`, `downloadFinished` |
| WebView2 | the engine's native chooser | the engine's native download UI | DevTools intercepts the chooser in tests; `DownloadStarting` redirects the download to a private path |

The engines require a user gesture to open a chooser, so the native scenarios click with a real pointer event (XTest under
Xvfb for WebKitGTK), a `Runtime.evaluate` with `userGesture` (CEF) or `evaluateJavaScript`, which WebKit treats as a user
gesture (WKWebView). The WKWebView scenarios (`WKWebViewLifecycle-download`, `-choose-file`, `-cancel-choose`,
`-replaced-download`) use the test injections of `WKWebViewOptions`: they qualify the delegate wiring, the blob download
policy, the destination handling and the replace-on-success logic, not the system `NSOpenPanel`/`NSSavePanel` themselves.
The real panels were driven by hand on macOS before the private staging change in `52d8c68` (accessibility actions, a
local slow server, no automated test): saving and the
"Replace" choice report the finished download and leave no `.part` file; cancelling the save sheet creates nothing; an
`<input webkitdirectory>` panel offers folders only; two simultaneous downloads of one name get `same.bin` and
`same (1).bin`; closing the window cancels the downloads in progress and removes their incomplete files; a response the
engine cannot display (`application/octet-stream`) is saved as a download.
These earlier manual results do not qualify the private staging implementation; the PR records subsequent validation
separately. Native dialog qualification remains incomplete.
WebKit reduces a suggested name such as `../x.txt` to `_x.txt` before the frontend sees it.
If all 10,000 candidate names are occupied, automatic destination selection fails and cancels the download.
WKWebView writes each download in a private staging directory beside the destination. On success it installs the file;
if a destination that was free appears during the download, it preserves that file and discards the download. Failed and
cancelled downloads remove their staging directory. A new destination is installed with a hard link, or by a copy that still refuses an existing file on filesystems without links (exFAT, network shares); a failed installation is reported on `stderr`. Staging directories left by a crash are removed after a week. Two downloads cannot target the same chosen file at once. The file chooser opens only for the application's own origin.

### Tests

`ConsumerJasmine-file-services` (all engines) checks that files reach the page as `File` objects, that the `accept` and
`webkitdirectory` properties keep their values (filtering and directory selection themselves are not exercised), and
that offering a generated Blob leaves the page and the bridge working. `WebKitGTKAdapter-download|choose-file|cancel-choose`
and `CEFFileServices-download|choose|cancel` drive the dialog and the download through the injection points.
`WebKitGTKAdapter-download-user-dir` checks that, without a configured folder, the download lands in GLib's user download
directory (read from `user-dirs.dirs`, not from an environment variable). `WebKitGTKAdapter-close-with-dialog` opens the
real system dialog with a pointer click, closes the window under it and requires a clean exit; CEF and WebView2 do not
test that case. `WKWebViewLifecycle-close-with-open-panel|close-with-save-panel` close the window under an
`NSOpenPanel` / `NSSavePanel` sheet, and `choose-in-blank|choose-in-srcdoc` open a chooser from `about:blank` and `srcdoc`
frames, which WKWebView judges by their inherited origin.
`WebView2Lifecycle-file-choose|file-cancel|file-download` verifies the engine's chooser request, selection and cancellation
events, and the bytes written by a completed Blob download. It intercepts the chooser with DevTools and suppresses the download UI,
so it does not qualify the native dialogs themselves or closing the window while one is open.

## Script dialogs

`alert`, `confirm` and `prompt` work in every embedded frontend, and `beforeunload` in all but WKWebView, without a WebFront API (ADR-020, rule 6, #452).
All four engines use the system's or the engine's own box attached to the window by default; each frontend also has a test
injection that answers without a box, with the shared types of `frontend/ScriptDialogs.hpp` (`ScriptDialog`,
`ScriptDialogAnswer`, `ScriptDialogHandler`). Closing the window with a box open never waits for an answer. Under WKWebView the sheet's completion handler answers dismissed (`false` for `confirm`, `null` for `prompt`);
under the other engines the page is gone with the window, and the value its call would have returned is not observable.

| Frontend | Dialogs | Test injection |
| --- | --- | --- |
| WebKitGTK | `script-dialog` signal; the default handler shows WebKitGTK's dialog on the window | `Session::answerScriptDialog` (`scriptDialogOpened` reports the default dialog) |
| CEF (Chrome runtime) | `CefJSDialogHandler` (`OnJSDialog`, `OnBeforeUnloadDialog`) answered only by the injection; otherwise the runtime's own dialogs | `SimpleCEFClient::answerScriptDialog` |
| WKWebView | the three `WKUIDelegate` panel methods (alert, confirm, prompt; not `beforeunload`, see below) show an `NSAlert` sheet of the window (an accessory text field for `prompt`); only frames whose security origin is the application's, others (including a sandboxed frame without `allow-same-origin`, whose origin is opaque even at the application's URL) are dismissed | `WKWebViewOptions::answerScriptDialog` |
| WebView2 | the engine's own dialogs, until the injection turns them off and answers `ScriptDialogOpening` (kinds alert, confirm, prompt, beforeunload) | `Session::answerScriptDialog` (adapter-internal, set from a `prepare` hook) |

Origin filtering differs: WKWebView answers dismissed for a frame outside the application's origin, because its code shows the
box itself. WebKitGTK, CEF and WebView2 leave the decision to the engine's own dialog (or to the injection), without a
WebFront origin check; the content policy that already restricts navigation to admitted origins is the guard there.

**Limit: closing the window does not ask `beforeunload`.** Closing the window itself is not a page navigation. Under WebView2
the window's `WM_CLOSE` ends in `ICoreWebView2Controller::Close`, which does not raise `beforeunload`; navigations and reloads
can reach the handler. Under WebKitGTK and CEF this was not verified. A page can therefore lose unsaved changes when the user
closes the window; the application must save its state itself (for example on `visibilitychange`).

**Limit: WKWebView does not handle `beforeunload`.** The public `WKUIDelegate` has no method for it; WebKit routes it through a
private selector (`_webView:runBeforeUnloadConfirmPanelWithMessage:initiatedByFrame:completionHandler:`), not through the confirm
method, and WebFront does not implement a private selector. A page that asks for a confirmation before leaving can therefore
lose unsaved changes under WKWebView; the application must save its state itself (for example on `visibilitychange`). This was
not verified on macOS: support through the private selector, if wanted, needs a decision and a check on a real system. The other
three engines report `ScriptDialog::Kind::beforeUnload`.

Tests: `WebKitGTKAdapter-script-dialogs` and `CEFFileServices-dialogs` check that each dialog reaches the injection with its
message and default text, that a confirmed `confirm` returns `true`, an accepted `prompt` returns its text and a dismissed
one returns `null`. `WebKitGTKAdapter-close-with-script-dialog` and `CEFFileServices-hold` close the window under an open
alert; `WKWebViewLifecycle-script-dialogs|close-with-script-dialog|sandboxed-script-dialog` (the last: a sandboxed opaque-origin frame's `confirm` is dismissed, never shown) and `WebView2Lifecycle-script-dialogs` cover the same
on their engines (the WebView2 case has no close-under-dialog scenario). No scenario triggers a real `beforeunload` (the
engines show it only after a user gesture on the page); its wiring is implemented but not exercised end to end.

## Adding a frontend

1. Implement the type in `include/webfront/frontend/` following the contract above and include it from `Frontend.hpp`. Keep platform headers behind the option's macro.
2. Add its support module and one `webfront_register_frontend(...)` entry in `cmake/Frontends.cmake`, using the fields below.
3. Extend `FrontendSelection` coverage and run the shared Jasmine and lifecycle suites. Enabling the engine on a `TEST_PLATFORMS` platform automatically registers the eight shared lifecycle cases; `WEBFRONT_TEST_FRONTEND` selects its C++ type in both runners.

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

`test/web/FrontendLifecycle.cpp` runs the same eight cases under the selected embedded engine:

- `no-bridge`: close a page that never connects to the bridge and return from `show()`.
- `early-close`: close immediately from the page's first script, before any bridge connection.
- `rejected-ready`: reject `webFront.ready` using an unreachable WebSocket endpoint, handle the rejection and close.
- `navigation-failure`: open an unreachable initial URL and require a navigation exception.
- `path-encoding`: load `encoded path #.html` and verify that its query and fragment survived.
- `content-policy`: block a popup and a foreign-origin top-level navigation while keeping the original page alive.
- `navigation-allow-list`: permit the same top-level destination through the explicit navigation allow-list.
- `view-association`: the page of the view connects with the secret of the launch and `UI::isEmbeddedView()` is true; a second connection of that page, from the same loopback address and without the secret, is not associated.

Each runs in a separate process as `<Frontend>Lifecycle-<case>`, with the registry's timeout, the
`web-integration` label and the same port lock as Jasmine. Linux CEF and WebKitGTK cases use Xvfb. On Linux CEF, run
`cmake --build <build> --target webtest developer_integration webfront-wf --parallel` to prepare the complete browser suite.
For other validated test platforms, build all targets to include engine-specific runners as well. Run
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

## Interface-thread executor

An embedded frontend runs a loop on the thread that called `show()`; services (the native dialogs of ADR-020) must run code
there. `app.uiExecutor()` returns a `frontend::UIExecutor`, a cheap handle: `available()` is true while the loop runs, and
`schedule()` is a sender completing on the interface thread (compose it with `ex::then`). Completions: `set_value()` on the
interface thread; `set_stopped()` when the receiver's stop token fires before the operation started, or when the window closes with it
pending (nothing runs after closure); `set_error(UIExecutorUnavailable)` when no loop is attached (before `show()`, after closure,
frontends without a loop). An operation that started is never interrupted, and `schedule()` always queues, even from the interface thread.

| Frontend | Wake-up |
| --- | --- |
| WKWebView | block on the main queue |
| WebView2 | window message `WM_APP + 0x4C3` |
| WebKitGTK | `g_idle_add_full` on the default context |
| CEF | `CefPostTask(TID_UI, ...)` |
| `BrowserWithUILoopFrontend` | message-only window (Windows), run loop source (macOS), GLib idle (Linux) |

The plain `DefaultBrowserFrontend` runs no loop and has no executor. `BrowserWithUILoopFrontend` launches the page in the system
browser and runs the minimal loop on the calling thread until `BrowserWithUILoopFrontend::requestStop()` (any thread); the server
then keeps running, as with the plain browser. On macOS it sets `NSApplication` to the Accessory policy (no Dock icon) only when
it is still Regular. A frontend author implements it with `UIExecutorHost`: `attach(wake)` on the interface thread, `drain()` when woken,
`close()` when the loop ends (`uiExecutorHostOf<Frontend>()` gives the process-wide host).

## Native window

The native dialogs of ADR-020 need the window that parents them. `app.nativeWindow()` returns a `frontend::NativeWindow` handle, whose `get()` gives a
`std::expected<NativeWindowHandle, NativeWindowError>`; `ui.nativeWindow()` is the window of one connection and returns that
`std::expected` directly, with no `get()`: `app.nativeWindow().get()` and `ui.nativeWindow()` are the two forms.
The handle carries `kind` (`win32`: `HWND`, `cocoa`: `NSWindow*`, `gtk`: `GtkWindow*`, `cef`: `CefBrowser*`), the `pointer`, and `portalParent`.

| Error | Meaning |
| --- | --- |
| `unsupported` | the frontend has no native window (system browser, test double) |
| `notEmbedded` | `ui.nativeWindow()` only: the connection is not the view of the frontend's window (remote client, system browser, loopback peer) |
| `notOpen` | the window is not created yet |
| `closed` | the loop ended; the window is gone |
| `wrongThread` | not called on the interface thread: toolkit objects belong to it |

Call it inside an operation scheduled with `app.uiExecutor()` and do not keep the pointer. The window stays published until the loop ends,
including after the user closed it, so check the object with the toolkit when that matters (`IsWindow`, `-[NSWindow isVisible]`).
The connection's window is the frontend's single window, reached through the association of ADR-020 rule 3 (`ui.isEmbeddedView()`); the
system browser never has one. `portalParent` is the `parent_window` of the desktop portals: under WebKitGTK `x11:<hex XID>` when the
window is on X11, `wayland:<handle>` once `gdk_wayland_toplevel_export_handle` succeeds (either is tried every 100 ms for 5 s after the window is
presented, until the surface exists and can be exported; empty until then); it is empty on Windows and macOS. A frontend author calls `nativeWindowHostOf<Frontend>().publish(...)` on the interface
thread after the window exists and `retract()` before it is destroyed. `publish()` returns the generation of the publication, and an
answer that completes later (the Wayland export) goes through `setPortalParent(text, generation)`, which drops it when its window was
withdrawn or replaced by a newer one.

Manual check of a parented dialog (to run on each platform with the first native backend): open a native dialog parented to
`ui.nativeWindow()` from an `app.uiExecutor()` operation; it must open over the application's window, stay on top of it, block it as
the platform does and return focus to it. No native dialog exists before the ADR-020 native backend; until then the manual program
`native_integration_manual` (`test/web/NativeIntegrationManual.cpp`) opens one itself. Run on Windows with WebView2 and with the
system browser's loop, and on Linux with the system browser's loop and WebKitGTK (X11 and Wayland); the `[look]` points of the dialog
(over the window, on top, blocking, focus returned) were confirmed by eye on Wayland and remain for a person to confirm on each other desktop. See the
[platform validation of #450](validation/native-integration-450.md).

## Native window with browser fallback

Without a preprocessor test, `webfront::embeddedFrontendAvailable` (`inline constexpr bool`, true when a native engine or CEF is
compiled in) and `webfront::EmbeddedWebFront` (always defined; the native web view when compiled in, otherwise CEF) tell an application what the build holds.
Without any engine, constructing `EmbeddedWebFront` throws `std::runtime_error`, so that case falls back with an ordinary `try`/`catch`.

What the constructor reports at run time depends on the engine, because each runs its own initialization at a different moment:

| Engine | Unusable engine or display is reported by |
| --- | --- |
| no engine compiled in | the constructor (`std::runtime_error`) |
| WebKitGTK, WKWebView | the constructor (`initialize()`: no display, missing runtime) |
| WebView2 | `show()`: `initialize()` is empty, a missing Evergreen runtime fails when the window opens |
| CEF | `show()`: `CefInitialize` runs in `open()` |

The constructor also builds the HTTP server, so a `catch` around it receives a bind or configuration error as well as a frontend
failure. The fallback then constructs a second server with the same configuration, which fails again with the same error
and propagates: nothing is hidden, but the attempt is repeated. Keep the `try` to the construction and serving outside it, so
an error of the application is never taken for a frontend failure:

```cpp
template <bool Embedded>
int run(const webfront::WebFrontConfig& config) {
    if constexpr (Embedded) {
        std::optional<webfront::EmbeddedWebFront> window;
        try {
            window.emplace(config);
        } catch (const std::exception&) { /* no usable engine: fall back to the browser */ }
        if (window)
            return serve(*window);
    }
    webfront::WebFront web{config};
    return serve(web);
}

int main() {
    return run<webfront::embeddedFrontendAvailable>(webfront::WebFrontConfig{});
}
```

This does not cover WebView2 and CEF failures, which surface from `show()` inside `serve`. An application that wants a fallback for them
catches around its own `show()` call and then serves with `WebFront`; it decides which errors count as a frontend failure.
For WebKitGTK and WKWebView, `frontend::NativeOrBrowserFrontend` below is the more precise tool: it intercepts only the failure
of the frontend's `initialize()`, which runs before the server is built, so no second server is constructed.

The selection is a function template on a `bool` because the condition of an `if constexpr` outside a template does not
depend on a template parameter, so its discarded branch would still be checked. Here the branch that names `EmbeddedWebFront`
is not instantiated when the argument is `false`, so a `serve` that does not accept that type compiles without an engine.

`frontend::nativeWebViewAvailable` is `true` when this platform's native engine is compiled in. It says
nothing about the session. `frontend::NativeOrBrowserFrontend` decides at run time: its `initialize()`
tries the native engine and, if that throws (engine not compiled in, no `DISPLAY`/`WAYLAND_DISPLAY`,
missing runtime), it initializes the browser frontend and uses the system browser for the whole process and prints the reason once to
`std::cerr`. Only `initialize()` triggers the fallback: if the native `open()` throws, the exception
propagates as for any frontend. `fallbackReason()` returns that reason and `usesNativeWindow()` tells which path was taken
once the application is constructed.

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::NativeOrBrowserFrontend>;
```

A native window stops the server when it closes; the browser path leaves it running until Enter, as
`DefaultBrowserFrontend` does. Because `action` is `constexpr`, the frontend reports this through the
optional `closeServerAfterOpen()` hook, which `show()` consults after `open()` returns.
`NativeOrBrowserFrontendFor<Native, Browser>` composes any two frontends the same way.

## Direct engine access

Every frontend currently loads its page over HTTP and exchanges bridge messages over a WebSocket. [ADR-017](adr/ADR-017-acces-direct-aux-frontends.md)
(proposed) describes how an adapter may later add its engine's native resource scheme and message channel without replacing that
path. Transport benchmarks are **manual measurement tools**, excluded from the default build,
CTest and every CI workflow. The targets are `webkitgtk_transport_benchmark`,
`webview2_transport_benchmark` and `wkwebview_transport_benchmark` for their respective engines.
Build one explicitly, then launch its executable (the macOS executable is inside its `.app` bundle).

For comparable Linux timings, use Release without coverage:

```bash
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_ENABLE_TESTING=ON \
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
`WEBFRONT_ENABLE_TESTING` and `WEBFRONT_ENABLE_FUZZING` disable the detection. A parent project's own `ENABLE_TESTING` or
`ENABLE_FUZZING` changes neither the detection nor WebFront's tests: only the prefixed options do.

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
| `WEBFRONT_EMBED_CEF` | OFF | Download and link CEF so CEFFrontend is available (Windows, Linux x86_64 and aarch64, and macOS only). Other platforms fail at configure time |
