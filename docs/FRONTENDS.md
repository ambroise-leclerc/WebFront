# Frontends

[Back to the README](../README.md#choose-how-your-interface-opens)

A frontend decides how the interface is displayed: the system browser (`DefaultBrowserFrontend`, the default), an embedded Chromium window (`CEFFrontend`) or an embedded native web view (`WebView2Frontend`). The application code and interface assets do not change when the frontend does. Platform-specific notes live next to each frontend, for example [WebView2](WEBVIEW2.md). This page states the rules they all follow; the C++ concept is `frontend::FrontendType` in `include/frontend/Frontend.hpp`.

## The contract

A frontend is a type with:

| Member | Meaning |
| --- | --- |
| `static void initialize()` | Called once per frontend type, when the first `BasicWF` of that type is constructed. |
| `static void open(std::string_view port, std::string_view file)` | Called by `show()` / `openAndRun()` on the calling thread, while the HTTP server runs on its own thread. |
| `static constexpr Action action` | What `BasicWF` does once `open()` returns. |

- **Blocking.** With `Action::closeServerAfterOpen` (embedded windows), `open()` blocks until the window is closed, releases everything it created, and returns; `BasicWF` then stops the server and joins its thread, so `show()` returns after a clean shutdown. With `Action::keepServerRunning` (the system browser), the server keeps running after `open()` returns. The built-in system-browser adapter waits for Enter in the console after launching the page; it cannot observe when the external browser closes.
- **Threading.** `show()` runs `open()` on the calling thread. Call `show()` from the application's main thread when you select an embedded frontend: AppKit and GTK require it and WebView2 needs an STA thread (an MTA thread is rejected with a clear error). The server never runs `open()`.
- **Errors.** A frontend reports failure by throwing from `open()`, whatever its action. `BasicWF` stops and joins the server before the exception propagates, so a failed start leaves no running thread behind.
- **Unavailable frontends.** Every frontend type is always declared. In a build without its support, initialization or opening throws a message naming the CMake option that enables it (`-DWEBFRONT_EMBED_CEF=ON`, `-DWEBFRONT_EMBED_WEBVIEW2=ON`).
- **URL.** Built-in frontends also provide `open(address, port, file)`, which `BasicWF` prefers when available. Build the URL with `frontend::detail::applicationUrl(address, port, file)` in `frontend/Url.hpp`: it uses the configured address and preserves URI query/fragment syntax and existing percent escapes. Spaces and UTF-8 bytes are encoded. For literal filename delimiters use `%23`, `%3F` and `%25`. The two-argument overload uses `127.0.0.1`; wildcard binds open their matching loopback address.
- **Selection.** `webfront::WebFront` always uses the system browser. An application opts into another frontend with `webfront::WebFrontWithFrontend<...>`; enabling an embedded engine at configuration time never changes the default.

## Trusted content

Embedded frontends currently target development with trusted application content: navigation and popup requests are not restricted, DevTools and context menus stay available, and nothing authorizes a page by origin before it uses the C++ bridge. A production application needs its own policy; a shared one is tracked in #260.

## Adding a frontend

1. Implement the type in `include/frontend/` following the contract above and include it from `Frontend.hpp`. Keep platform headers behind the option's macro.
2. Add its support module and `WEBFRONT_EMBED_<NAME>` option (default `OFF`), then one entry in `cmake/Frontends.cmake`; the demo (`WEBFRONT_DEMO_FRONTEND`) and browser test (`WEBFRONT_WEBTEST_FRONTEND`) selectors pick it up from there.
3. Run the shared Jasmine suite with it and add lifecycle tests: closing a page that never connects, closing early, a rejected `webFront.ready`, a failed initial navigation, and a page path containing a space and `#`.
