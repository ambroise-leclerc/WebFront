# WebView2 frontend

This page covers what is specific to WebView2. The rules shared by every frontend are in [Frontends](FRONTENDS.md).

[Back to the README](../README.md#choose-how-your-interface-opens)

The **Microsoft Edge WebView2 Evergreen Runtime** must be installed separately. If it is absent, opening throws an actionable error before creating a window. App installers can redistribute Microsoft's small online Evergreen bootstrapper or the offline Evergreen standalone installer; see [Microsoft's runtime distribution guide](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution). Fixed-version runtime packaging is outside this adapter's scope.

Follow the [shared main-thread contract](FRONTENDS.md#the-contract). WebView2 additionally needs STA-compatible COM; an existing MTA apartment is rejected. The adapter balances COM initialisation, owns the message loop, resizes the view with its window and applies `WM_DPICHANGED`'s suggested bounds. It handles JavaScript `window.close()` and releases its controller on shutdown. Closing is idempotent; a failed controller `Close()` is reported by `show()` without replacing an earlier error. Late callbacks after the session has ended can only release their own controller references.

Profiles default to `%LOCALAPPDATA%/WebFront/<executable-path-hash>/WebView2`, so the executable directory can be read-only. The FNV-64 executable-path hash is case-sensitive: paths differing only by case can select separate profiles. Instances of the same executable share that profile. It is persistent and is not automatically deleted; select another `userDataFolder` through the options below when isolation is needed.

Runtime creation failures and failed initial navigation throw. Closing during startup cancels the startup timer and returns once the window is destroyed, even if a creation callback never arrives; an existing error is preserved. Late creation callbacks hold weak references. The runtime watchdog stops before initial navigation starts, so slow page resources do not consume its deadline. Initial navigation is tracked separately: a failed initial navigation still throws, while errors after a successful first navigation leave the window open. A page without a connected bridge can still be closed normally.

An intentional navigation cancellation is tolerated. WebView2 can also report `CONNECTION_ABORTED` before delivering a page's `window.close()` request; when the navigation already received an HTTP success or redirect response, this is treated as normal cancellation. A connection failure without such a response still throws.

A temporarily unresponsive renderer is tolerated. Browser or main renderer process exits deliberately end the session with an error. Automatic reload is not attempted because reloading can repeat application actions; the caller decides how to recover.

Window destruction ends this session's message loop directly, without posting a thread-wide `WM_QUIT`. This allows a subsequent `show()` on the same thread; an existing external `WM_QUIT` is preserved for the owning application.

The adapter does not supply a DPI-awareness manifest. Applications should provide their own manifest for sharp rendering on HiDPI displays; without one Windows may scale the window and make it blurry.

The [shared content policy](CONTENT_POLICY.md) defines the defaults, configuration and limitations for navigation, popups, bridge origins, DevTools and context menus. WebView2 applies it through `NavigationStarting`, `NewWindowRequested` and engine settings. Direct callers can set `WebView2Options::contentPolicy`; `BasicWF` forwards `WebFrontConfig::contentPolicy` automatically, including with `WebView2FrontendWithOptions`.

The adapter implementation lives in `frontend::detail::webview2` and is not a supported public API.

## Runtime timeout and profile options

The runtime-creation deadline defaults to 30 seconds. Increase it for cold starts or machines with slow antivirus scanning. It must be a positive duration, no greater than Windows' `USER_TIMER_MAXIMUM` milliseconds. A policy supplies options for each opening without mutable process-wide settings:

```cpp
struct DesktopOptions {
    static webfront::frontend::WebView2Options get() {
        return {.startupTimeout = std::chrono::minutes{2},
                .userDataFolder = "my-application-profile"};
    }
};

using Desktop = webfront::frontend::WebView2FrontendWithOptions<DesktopOptions>;
using App = webfront::WebFrontWithFrontend<Desktop>;
```

An empty `userDataFolder` keeps the default. Relative paths are resolved against the working directory; the selected directory must be writable. Callers manage the lifetime and eventual removal of custom profiles. Direct frontend users can also pass `WebView2Options` to `WebView2Frontend::open(address, port, page, options)`.

## Addresses and page references

All built-in frontends use the address in `WebFrontConfig`, rather than hard-coded `localhost`. IPv6 addresses are bracketed in the URL; wildcard binds (`0.0.0.0` and `::`) open the matching loopback address. The listener is bound and listening during application construction, before the frontend opens.

The default address is `127.0.0.1`, so the browser reaches the IPv4 loopback listener directly without relying on `localhost` resolving or falling back to IPv4.

The page is a UTF-8 URI reference. `show("index.html?x=1#result")` retains its query and fragment; spaces and non-ASCII bytes are percent-encoded. Already encoded `%XX` sequences are preserved. For a filename containing literal `#`, `?` or `%`, use `%23`, `%3F` or `%25`, for example `show("encoded path %23.html")`. CEF, WebView2 and the system browser share this construction.

A literal `+` in the path is automatically encoded as `%2B`: `show("a+b.html")` serves the filename `a+b.html`. This avoids the server decoder's conversion of an unescaped `+` to a space. Plus signs in the query and fragment retain their original meaning.

The shared Jasmine suite opens `encoded path %23.html` under both CEF and WebView2 and checks the decoded filename, query and fragment. Catch2 also checks this path at the system-browser launcher boundary and verifies shell quoting. On Windows the system browser is opened directly with `ShellExecute`, without a command shell; POSIX launches quote the URL as one argument.
