# WKWebView frontend (macOS)

`WKWebViewFrontend` opens a native AppKit window with the WebKit engine supplied by macOS.
It is optional and never changes `webfront::WebFront`'s system-browser default.

To run the Game of Life developer example on the pinned developer environment, from the repository root:

```bash
cmake -S . -B build-wkwebview -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WKWEBVIEW=ON
cmake --build build-wkwebview --target WebFrontLifeDeveloper --parallel 2
mkdir -p life-project
./build-wkwebview/examples/WebFrontLifeDeveloper "$PWD/life-project"
```

To run an environment built from the sources instead, [build the local archive](DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers) and pass it:

```bash
mkdir -p life-project/.webfront/developer/preview/provision
cp build-developer-environment/*.tgz life-project/.webfront/developer/preview/provision/
integrity=$(sed -n 's/^  "integrity": "\(.*\)",$/\1/p' build-developer-environment/provision.json)
./build-wkwebview/examples/WebFrontLifeDeveloper "$PWD/life-project" \
  /webfront/developer/preview/provision/environment.tgz "$integrity"
```

For HelloWorld or your application, select the adapter in C++:

```cpp
#include <webfront/WebFront.hpp>
using App = webfront::WebFrontWithFrontend<webfront::frontend::WKWebViewFrontend>;

int main() {
    App app;
    app.show();
}
```

The type is always declared. Selecting it without support throws a diagnostic naming
`-DWEBFRONT_EMBED_WKWEBVIEW=ON`. Enabling that option outside macOS fails during configuration.
There is no engine download, SDK package or separate runtime installation.

## Implementation and lifetime

The adapter stays header-only under `include/webfront/frontend/WKWebView.hpp`. It sends public Cocoa/WebKit
messages through the Objective-C runtime and registers delegate/view classes once. It compiles as
C++23, without an Objective-C++ `.mm` source or ARC. Clang's Blocks extension (`-fblocks`, inherited
through `WebFront::WebFront`) supplies WebKit's decision-handler calling convention. CMake links
the system Cocoa and WebKit frameworks. Both Apple Clang and upstream Clang are supported.

macOS **13.3** is the minimum, including when specifying `CMAKE_OSX_DEPLOYMENT_TARGET`.
Initialization checks the running OS before creating UI. This baseline makes the public
[`inspectable`](https://developer.apple.com/documentation/webkit/wkwebview/isinspectable) property
available without private preferences. Construct the application and call `show()`/`open()` on
the **main thread**; initialization and every opening check this and throw actionable errors.

`open()` creates the shared `NSApplication` on first use, installs a window, resizable `WKWebView`
and an Application/Edit menu with Quit and Cut/Copy/Paste/Select All shortcuts. It owns the AppKit
run loop until native closure, `window.close()` or Quit. It stops that loop without terminating
the process, removes delegates/timers and releases the window/view before returning. It restores
the previous application delegate, menu and activation policy; the process-wide `NSApplication`
and registered classes remain available for later openings. The same runtime classes are reused
when the header is present in both an executable and a dylib. Images defining their method
implementations must remain loaded until process exit. Successive applications can call `show()`
in the same process; each stopped application owns its own server lifetime.
Nested openings and opening inside an already-running AppKit loop are rejected.

A host bundle with `LSUIElement=true` keeps the accessory activation policy while its window
is open: it can display UI without adding a Dock icon or owning the application menu bar. Ordinary applications use the regular
activation policy. Both restore the host's previous policy when the session closes.
The WKWebView browser-test executables are identified agent bundles so repeated CTest runs
do not add anonymous executable entries to the Dock.

The application delegate always returns `NSTerminateCancel`, so Cmd-Q closes the WebFront session
and returns control to C++. It does not distinguish a user Quit from logout or system shutdown;
macOS may report that the application interrupted shutdown. Hosts needing system-termination
coordination must provide that integration separately.

The URL comes from the shared application-URL helper, including address selection, percent escapes,
query and fragment. WebFront's existing HTTP/WebSocket bridge is the only bridge; there is no
`WKScriptMessageHandler` and the frontend never waits for a bridge connection to close its window.
Pages without the bridge and pages handling a rejected `webFront.ready` can close normally.

Initial network/HTTP errors and web-content-process termination close the session and throw.
An internal error document finishing without a valid HTTP response is also a failure.
The initial document has a 30-second loading deadline, cancelled after successful navigation;
later navigation errors leave the session open. Direct callers can override the deadline with
`WKWebViewOptions::startupTimeout` (positive milliseconds). This is a loading deadline, not a
deadline for the application's bridge or asynchronous JavaScript work.
The timer is scheduled in the default run-loop mode. Menu tracking or interactive resizing can
delay its firing; this is not a strict wall-clock limit while those interactions are active.

The fourth argument to `open(address, port, page, ...)` accepts a `ContentPolicy` or a typed
`WKWebViewOptions`. An empty `{}` selects the default content policy without overload ambiguity.

## Website data and content policy

Each opening configures a fresh
[`nonPersistentDataStore`](https://developer.apple.com/documentation/webkit/wkwebsitedatastore).
Cookies, caches and local storage are not intentionally persisted between windows or shared across
executables. There is no profile directory to manage for unbundled command-line applications.

The adapter enforces the [shared content policy](CONTENT_POLICY.md) for top-level navigation,
including redirects, and cancels unmanaged popup creation. The server applies bridge-origin rules
unchanged. DevTools remain enabled by default for trusted development content: select the view in
Safari's Develop menu. `devToolsEnabled = false` disables inspection; `contextMenusEnabled = false`
clears the view's native context menu.

Subframe navigation remains outside this top-level policy, consistently with CEF's `IsMain()`
check and WebView2's main-frame `NavigationStarting` handler (no frame-navigation handler is
registered). The shared server-side bridge-origin check still applies to frames that connect.

**Popup limitation:** WebKit's public `WKNavigationAction` API does not provide a reliable
user-activation flag equivalent to CEF's `user_gesture` or WebView2's `IsUserInitiated`.
WKWebView therefore blocks all popups, including when `PopupPolicy::systemBrowser` is requested,
instead of forwarding script-triggered URLs. It uses no private WebKit API.

CEF and WKWebView can both be linked into a binary; availability does not initialize either.
Only the selected frontend's `initialize()` runs. Both use the process-wide `NSApplication`;
simultaneous sessions or switching between already-running toolkit loops are unsupported.

## Bundles and local networking

This frontend targets unbundled executables. App bundling, signing, sandbox entitlements and iOS
are outside its scope. For an executable placed in an `.app`, configure its own `Info.plist`
according to Apple's
[`NSAllowsLocalNetworking`](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)
documentation. Ensure ATS permits the local HTTP page and its `ws://` bridge;
do not disable ATS globally for arbitrary hosts. A narrowly scoped configuration is:

```xml
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
</dict>
```

On the local macOS 27 ARM64 verification host, the full Jasmine suite passed from a temporary
`.app` both without an ATS dictionary and with the local-networking key above, including HTTP
module/assets and the `ws://` bridge. An ATS exception was not necessary for loopback on that
host; this does not establish the behavior of every supported OS/SDK pairing.

ATS behavior depends on the OS, SDK linkage and bundle configuration. WebFront does not edit
the host application's plist. Loopback access is distinct from local-network privacy permissions
for LAN hosts; the default address remains loopback.

## Unattended verification

```bash
cmake -S . -B build-wkwebview -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WKWEBVIEW=ON \
  -DENABLE_TESTING=ON -DWEBFRONT_WEBTEST_FRONTEND=WKWebView
cmake --build build-wkwebview --parallel
ctest --test-dir build-wkwebview --output-on-failure
```

WKWebView does not advertise Brotli on local HTTP connections. The bundled compressed assets
use gzip; their decoded contents are unchanged. See [HTTP asset encoding](HTTP_ASSETS.md)
for the shared server negotiation, fallback errors and configurable decoded-byte budget.

The shared Jasmine suite exercises both bridge directions, errors, readiness and native modules.
The shared lifecycle cases cover pages without a bridge, early closure, rejected readiness,
navigation failure, encoded paths and navigation/popup policy. A commit-state regression keeps
an interactive document alive when a later navigation is denied before slow subresources finish. Additional native cases cover
repeated openings, HTTP errors, main-thread rejection, Quit, native early closure, a stalled HTTP
load deadline, denied/allow-listed HTTP redirects, data-store isolation and inspector/context-menu settings. A second C++ translation
unit includes the standalone header to check ODR safety and transitive compile/link requirements.
Separate executable/dylib tests force independent inline caches, exercise both registration orders,
and close a dylib-owned session using the shared runtime delegates.

Tests require a logged-in WindowServer session. The dedicated `macos-26` CI job checks that
session before running the suite, with CTest timeouts and a job deadline. It does not require
user interaction or an existing browser session.

The manual target `wkwebview_transport_benchmark` uses that same window adapter to compare the
HTTP/WebSocket bridge with `WKScriptMessageHandlerWithReply`, `evaluateJavaScript`, `callAsyncJavaScript`, and a base64
byte envelope at 16 B, 1 KiB and 32 KiB. It checks each echo, reports medians without timing thresholds, and writes
`test/web/wkwebview-transport-benchmark.json` in the build tree. Run it alone, without a concurrent build, for measurements:

```bash
cmake --build build-wkwebview --target wkwebview_transport_benchmark --parallel 2
cd test/web
../../build-wkwebview/test/web/wkwebview_transport_benchmark.app/Contents/MacOS/wkwebview_transport_benchmark /tmp/wkwebview-transport-benchmark.json
```

`WKWebViewTransportContracts` verifies native rejections, abandoned replies, Unicode/NUL strings, binary base64
round trips, Promise results, JavaScript errors, incorrect echoes and refusal of same-origin and opaque subframes.
These channels exist only in the manual benchmark and the contracts-only test executable.
Timed measurements are excluded from CTest and CI. [ADR-017](adr/ADR-017-acces-direct-aux-frontends.md) records
their measurements and the consequences of Apple's public APIs for a future direct transport, including the
Foundation type boundary, handler lifetime, origins and custom resource schemes. iOS remains unverified.
