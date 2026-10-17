# WebView2 frontend

This page covers what is specific to WebView2. The rules shared by every frontend are in [Frontends](FRONTENDS.md).

[Back to the README](../README.md#choose-how-your-interface-opens)

The **Microsoft Edge WebView2 Evergreen Runtime** must be installed separately. If it is absent, opening throws an actionable error before creating a window. App installers can redistribute Microsoft's small online Evergreen bootstrapper or the offline Evergreen standalone installer; see [Microsoft's runtime distribution guide](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution). Fixed-version runtime packaging is outside this adapter's scope.

Call `show()` on a thread compatible with STA COM (normally the main thread); an existing MTA apartment is rejected. The adapter balances COM initialization, owns the message loop, resizes the browser with its window, handles JavaScript `window.close()` and releases its controller on shutdown. Profiles are stored under `%LOCALAPPDATA%/WebFront/<executable-path-hash>/WebView2`, so the executable directory can be read-only. The FNV-64 executable-path hash is case-sensitive: paths differing only by case can select separate profiles.

Startup failures, including failed initial navigation, throw. Closing during startup cancels the startup timer and returns once the window is destroyed, even if a creation callback never arrives; an existing error is preserved. Late creation callbacks hold weak references. Once the initial navigation succeeds, later navigation errors leave the window open. A temporarily unresponsive renderer is tolerated; browser or main renderer process exits remain fatal. Automatic page reload after a renderer exit is not implemented. A page without a connected bridge can still be closed normally. The page argument is a UTF-8 path, percent-encoded in the local HTTP URL, including spaces and literal `#`, `?` and `%` characters.

The adapter does not supply a DPI-awareness manifest. Applications should provide their own manifest for sharp rendering on HiDPI displays; without one Windows may scale the window and make it blurry.

This experimental frontend is intended for development with trusted application content. It does not filter `NavigationStarting` by origin or intercept `NewWindowRequested`: external links can replace the application page, and `window.open()` can create windows outside `Session` management. It supplies no origin-based authorization for the C++ bridge. DevTools and default context menus remain enabled for development. A production host needs its own navigation, popup and bridge authorization policy; see [Microsoft's WebView2 security guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security).

The adapter implementation lives in `frontend::detail::webview2` and is not a supported public API.

The initial-navigation watchdog is currently a fixed 30-second internal constant. A slow first runtime launch, including antivirus scanning, can exceed it. Public timeout configuration is not available yet.

Every frontend builds its URL with the shared `frontend::detail::applicationUrl()`: `http://localhost:<port>/` followed by the page path, with `/` kept literal and every other byte except RFC 3986 unreserved characters percent-encoded, so filenames containing spaces or `#` behave the same in the default browser, CEF and WebView2. A server bound only to `127.0.0.1` relies on the browser falling back to IPv4 if `localhost` resolves to IPv6 first; the adapter does not control this resolution order.
