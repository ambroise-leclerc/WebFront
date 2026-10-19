# Content policy

`WebFrontConfig::contentPolicy` is a per-application `webfront::ContentPolicy`, copied into the
HTTP server and passed to built-in frontends for each opening. It applies to the system browser,
CEF, WebView2 and WKWebView as described below. This experimental library still targets trusted application
content; origin checks do not make arbitrary application scripts safe.

## Defaults and migration

The defaults are tightened in #260: the bridge accepts the configured application origin,
embedded top-level navigation stays on that origin, and embedded popups are blocked. DevTools
and context menus remain enabled for development. Native clients that omit `Origin` remain
allowed; set `allowMissingOrigin = false` to require it. There is no authentication of native
clients: they can forge this header. This is browser cross-origin protection, not a credential.

| Member | Default | Meaning |
| --- | --- | --- |
| `bridgeAllowedOrigins` | empty | Additional HTTP(S) origins allowed to open a bridge WebSocket. |
| `navigationAllowedOrigins` | empty | Additional HTTP(S) origins allowed in the embedded top-level window. This does not grant bridge access. |
| `allowAnyBridgeOrigin` | `false` | Explicit development opt-out accepting any well-formed HTTP(S) origin. |
| `allowAnyNavigation` | `false` | Explicit development opt-out allowing any HTTP(S) top-level navigation. |
| `allowMissingOrigin` | `true` | Accept clients without `Origin`. |
| `popups` | `PopupPolicy::block` | Cancel new windows; `systemBrowser` forwards user-initiated HTTP(S) destinations to the system browser instead. |
| `devToolsEnabled` | `true` | Enable embedded engine DevTools entry points. |
| `contextMenusEnabled` | `true` | Enable embedded default context menus. |

```cpp
webfront::WebFrontConfig config;
config.contentPolicy.bridgeAllowedOrigins = {"http://localhost:5173"};
config.contentPolicy.navigationAllowedOrigins = {"https://docs.example.com"};
config.contentPolicy.popups = webfront::PopupPolicy::systemBrowser;
config.contentPolicy.devToolsEnabled = false;
config.contentPolicy.contextMenusEnabled = false;
webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend> app(config);
app.show();
```

Allow-list entries must be origins (`scheme://host[:port]`), without paths, trailing slashes,
credentials, queries, fragments or wildcards. Invalid configuration throws `std::invalid_argument`
before the listener is bound. HTTP/HTTPS schemes and hosts are compared case-insensitively and
default ports are normalized. IPv6 uses bracketed addresses; use the same address serialization
as the browser. Different hostnames (including `localhost` versus `127.0.0.1`), ports and schemes
are different origins, even when they reach the same machine. Wildcard binds (`0.0.0.0`, `::`)
use their matching loopback origin; explicitly list any additional origin used to access the app.
The request's `Host` header never grants bridge permission.

Every WebSocket upgrade is checked before the server sends `101` or creates a `WebLink`.
Disallowed, malformed, opaque (`null`) and duplicate `Origin` headers receive `403 Forbidden`.
The development opt-out still rejects malformed and opaque origins and duplicates. An ordinary
HTTP request is unaffected. No JavaScript bridge protocol changes are needed. A browser sees
its usual WebSocket connection failure, so `webFront.ready` rejects; applications must handle it.
The refusal includes a plain-text HTTP body and a warning log pointing to
`contentPolicy.bridgeAllowedOrigins`, `allowMissingOrigin` and the loopback-only wildcard-bind
default. The browser WebSocket API does not expose that HTTP response body to JavaScript;
inspect the native log or network diagnostics when debugging a connection from another host.

## Frontend mapping and scope

| Frontend | Navigation and popups | DevTools and context menus |
| --- | --- | --- |
| System browser | The external browser owns navigation and new windows; the adapter cannot intercept them. Server bridge checks still apply. | Controlled by the user's browser. |
| CEF | `OnBeforeBrowse` checks top-level navigations, including redirects; `OnOpenURLFromTab` handles new-tab requests, and `OnBeforePopup` always cancels unmanaged windows. | Context-menu filtering and Chrome command handling control DevTools; context menus can be cleared. |
| WKWebView | Navigation action/response delegates check top-level destinations; unmanaged popup creation is cancelled. All popups are blocked, including `systemBrowser`, because the public API lacks a reliable user-activation flag. | Public `inspectable` controls Safari inspection; a view override clears disabled context menus. |
| WebView2 | `NavigationStarting` cancels disallowed top-level navigations, including redirects; `NewWindowRequested` is always marked handled. | `AreDevToolsEnabled` and `AreDefaultContextMenusEnabled` settings. |

Same-origin page references, query strings and fragments continue working. A denied later
navigation leaves the current page alive. Popup forwarding intentionally does not use the
navigation allow-list: it opens outside the embedded window and grants no bridge access. Only
HTTP(S) destinations can be forwarded; `file:`, `javascript:`, `data:` and opaque URLs are blocked.
Forwarding requires the engine's user-initiation flag (`user_gesture` in CEF and
`IsUserInitiated` in WebView2); repeated script-only popup requests never launch the system browser.
On Windows the launcher converts UTF-8 URLs to UTF-16 for `ShellExecuteW`.
Popup launch failures are logged by both adapters and leave the application session running.
WebView2 still reports failures to read popup event data or mark it handled as session errors.

This is a top-level navigation policy, not a subresource/iframe/network sandbox. Frames, fetches,
third-party scripts, downloads, OS protocol handlers and browser extensions need separate host
controls. A same-origin script already has the application's bridge authority. The server exposes
no TLS or user authentication. Keep the default loopback bind unless remote access is deliberate.
DevTools settings control the adapter's engine UI, not inspection by native host code or an external debugger.

Custom frontends can opt into `open(address, port, page, const ContentPolicy&)`; `BasicWF` prefers
it over the legacy three- and two-argument overloads. Custom adapters are responsible for mapping
the policy to their engine; server bridge checks apply even with legacy adapters. Future WebKitGTK adapters should map these same rules to their navigation and window-creation delegates.

## Regression coverage

Catch2 checks exact origin matching, normalization, allow-lists, absent/malformed/duplicate headers,
HTTP status and popup delegation. Jasmine verifies the normal same-origin bridge and real denied
connections from a foreign-origin iframe and a sandboxed opaque origin, bracketed by positive
connections to the same endpoint. Rejected probes report only after the WebSocket close event,
before their parent removes their frame. Positive controls run in the original test document
and remain open until suite teardown.
Shared CEF/Xvfb, WebView2 and WKWebView lifecycle cases use native navigation cancellation notifications
and destination-page reports to exercise blocked popups/navigation and an explicitly
allowed navigation, without fixed scheduling delays. A CEF runtime probe checks every DevTools
command ID, menu filtering, user-gesture popup/new-tab forwarding and actual DevTools creation via `OnBeforeDevToolsPopup`, then closes
DevTools and verifies that the application can still navigate. DevTools use a separate client so
their internal navigation and closure do not inherit the application's policy or lifetime handlers.
WebView2 COM mocks check user-initiation flags, both engine settings and non-fatal launcher errors.
Windows launcher tests verify UTF-16 delivery without opening a desktop browser. Handshake tests
check the refusal body, its length and the warning log. All browser tests are unattended,
timeout-bounded and report a nonzero status on failure. See [frontend test commands](FRONTENDS.md#lifecycle-tests).
