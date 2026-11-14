# Adding WebFront to an application that has its own loop

[Back to the README](../README.md#contracts-and-limitations)

`app.show()` keeps the thread that calls it: the system browser frontend waits for Enter in the console, and an
embedded window runs its toolkit loop until the window closes. An existing application that already has an event loop
or worker threads does not have to give that thread away. This page gives the three ways to integrate, which frontends
allow each, the threads involved and how the host stops WebFront. The frontend contract itself is in
[FRONTENDS.md](FRONTENDS.md); the thread rules of the bridge are in [ASYNC_EXECUTION.md](ASYNC_EXECUTION.md#threads).

## Three threads, not one

| Thread | What runs there |
| --- | --- |
| Server thread | The HTTP/WebSocket server. `show()` and `start()` create it; it runs every `cppFunction` callback, `onUIStarted` and subscription handler, DOM event callback and task body, one at a time. |
| Interface thread | The loop of an embedded window (or of `BrowserWithUILoopFrontend`): the thread that calls `show()`. The plain system browser has none. `app.uiExecutor()` schedules work on it. |
| Host threads | Everything else in the application. They call WebFront through the calls documented as callable from any thread: `ui.jsFunction`, `app.broadcast`, `app.forEachUI`, `app.spawn`, `app.requestClose`, `app.stop`. |

The blocking of `show()` comes from the interface, never from the server: the server always has its own thread.

## Which path for which frontend

| Frontend | Returns to the host: `start()` | Secondary thread: `UIThread` | Main thread: `show()` + `requestClose()` |
| --- | --- | --- | --- |
| `DefaultBrowserFrontend` (`webfront::WebFront`) | **yes** | not needed (refused: nothing to close) | `show()` waits for Enter; use `start()` |
| `BrowserWithUILoopFrontend` | no (it exists for its loop) | Windows and Linux; refused on macOS | yes; `requestClose()` ends the loop |
| `WebView2Frontend` | no | **yes** (the thread becomes an STA) | yes |
| `WKWebViewFrontend` | no | refused: AppKit belongs to the main thread | **yes** |
| `WebKitGTKFrontend` | no | refused: GTK belongs to the main thread | **yes** |
| `CEFFrontend` | no | refused: not a qualified configuration | **yes** |
| `NativeOrBrowserFrontend` | no | refused: its browser fallback waits for Enter and cannot be closed | yes for the native window; its browser fallback waits for Enter |

"Refused" is a compile-time diagnostic that names this page: `start()` on a frontend without `launch()`, and
`UIThread` on a frontend whose `requiresMainThread` is true or whose sessions cannot all be ended by `requestClose()`
(`frontend::endsOnRequest`). An adapter also checks its
toolkit at run time: WKWebView and WebKitGTK throw a `std::runtime_error` that says "main thread" when constructed or
opened elsewhere, and WebView2 refuses a thread that already uses MTA COM. CEF makes no such check; do not move it.

On Linux, `BrowserWithUILoopFrontend` runs GLib's default main context. A host that already runs that context on
another thread (a GTK or GLib application) owns it: keep the plain system browser and `start()` there.

## 1. Return to the host: `start()`

With the system browser there is no window to own, so nothing has to block. `start()` starts the server thread,
opens the page and returns; it reads nothing from the console.

```cpp
webfront::WebFront app;
std::atomic<bool> stopRequested = false;
app.cppFunction("stopHost", [&] { stopRequested = true; });  // server thread

app.start();                                 // returns at once; throws if the browser cannot be launched

const auto onTick = app.broadcast("hostLoop.onTick");
for (std::int32_t tick = 0; !stopRequested; ++tick) {  // the host's own loop
    onTick(tick);
    std::this_thread::sleep_for(100ms);
}
app.shutdown();                              // stops and joins the server
```

[`examples/host-loop/HostLoop.cpp`](../examples/host-loop/HostLoop.cpp) is this program, complete (`WebFrontHostLoop`).

- **Readiness.** After `start()` returns the server is listening (`app.port()`); a browser has connected when the
  `onUIStarted` handler or a `subscribeUIStarted` subscriber runs. WebFront cannot observe that the external browser
  opened or closed a tab other than through its connections (`subscribeUIClosed`).
- **Failure.** If the launch throws, the server is stopped and joined before the exception reaches the caller.
- **Stop.** `app.shutdown()` or the destructor, from a host thread, outside any WebFront callback. `app.stop()` only
  requests it and may be called from anywhere.
- Register callbacks and `onUIStarted` before `start()`. Call it once per application, on the thread that owns it,
  instead of `show()`: a second `start()`, or mixing it with `show()`, throws `std::logic_error` and leaves the
  running server untouched.
- The launched browser gets the address, the port and the page (and the content policy, if the frontend's `launch()`
  takes it). Like `show()` with the system browser, it gets no window options and no view-association proof.

## 2. A thread for the interface: `UIThread`

An embedded window needs a thread for its loop. When the frontend accepts a thread other than the main one,
`webfront::UIThread<App>` (`<webfront/UIThread.hpp>`) owns that thread: it constructs the application there (the
frontend's initialization has the same thread affinity as its window), runs the host's `setup`, then `show()`.

```cpp
#include <webfront/UIThread.hpp>
using App = webfront::WebFrontWithFrontend<webfront::frontend::WebView2Frontend>;

webfront::UIThread<App> ui(webfront::WebFrontConfig{}, [&](App& app) {   // on the interface thread
    app.cppFunction("stopHost", [&] { stopRequested = true; });
});                                           // returns once the application is set up; rethrows a start failure

const auto onTick = ui.app().broadcast("hostLoop.onTick");
while (!stopRequested && ui.running())        // the user may close the window
    hostLoopIteration(onTick);

ui.stop();                                    // closes the window, joins the thread, rethrows what show() threw
```

- **Readiness and failure.** The constructor returns when the application is constructed and `setup` has run, and
  rethrows on the host thread what either threw (a missing runtime, an engine that is not compiled in). A failure of
  the opening itself comes later: `running()` turns false, `failure()` holds the exception and `stop()` rethrows it.
- **Exchanges.** `ui.app()` is the application; use the calls callable from any thread. The `UIThread` object itself
  belongs to one host thread: `app()`, `requestClose()` and `stop()` are not synchronized with one another. Callbacks run on the server
  thread, which is neither the host's nor the interface thread.
- **The user closes the window.** `show()` returns and stops the server; `running()` is false and `waitFor()`
  returns true. The application object stays alive until `stop()` or destruction, so references the host kept
  (a `UI`, a broadcast function) remain valid: their calls are dropped, typed calls fail.
- **Stop.** `stop()` asks the window to close, repeating the request while the window is still opening, joins the
  thread and destroys the application. If the window has not closed after its `patience` (30 s by default) it throws
  `webfront::UIThreadStalled` and changes nothing: the session still runs and `stop()` can be called again. The
  destructor does the same as `stop()` but cannot leave its thread behind: it waits without bound and drops a failure. No thread is detached and
  none outlives the object. Do not call `stop()` or destroy the object from a WebFront callback or from `setup`:
  both would wait for the thread they run on.

## 3. The interface owns the main thread: `show()` and `requestClose()`

AppKit and GTK give the application no choice: the window lives on the process main thread. The host then turns
around: the main thread constructs the application and calls `show()`, and the host's own loop runs on a thread it
creates, or as work scheduled on the interface thread.

```cpp
#include <atomic>
#include <chrono>
#include <thread>

using App = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
App               app;                        // main thread
std::atomic<bool> showReturned{false};
std::jthread host([&](std::stop_token stopping) {
    const auto onTick = app.broadcast("hostLoop.onTick");
    while (!stopping.stop_requested() && hostLoopIteration(onTick)) { }
    // The host may finish before the window exists: the request reaches nothing and is not kept, so repeat it.
    while (!showReturned && !stopping.stop_requested()) {
        app.requestClose();                   // any thread: show() returns as if the user had closed the window
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
});
app.show();                                   // main thread, until the window closes
showReturned = true;
host.request_stop();                          // the user closed the window first
```

- `requestClose()` never blocks. It returns false when the request reached nothing: no window is open yet, it already
  closed, the frontend has no window of its own, or the window is another application's. A frontend is process-wide, so
  an application asks it only while its own `show()` runs and, when the frontend publishes its interface thread, only
  if that thread is the one `show()` runs on: an application that shows nothing, or whose `open()` was refused
  because another session exists, never closes that session. A host that may ask before the window exists repeats the request
  until `show()` has returned, as `UIThread::stop()` does. The window closes without asking the page (`beforeunload`).
- Host work that must touch the toolkit runs on the interface thread through `app.uiExecutor().schedule()`, which is
  available while `show()` runs ([UIExecutor](../include/webfront/frontend/UIExecutor.hpp)).
- A host that owns a native loop of its own on the main thread (its own `NSApplication` run, its own GTK main loop)
  cannot share it with an embedded WebFront window today: `show()` runs the loop itself. Use the system browser and
  `start()` in that application, or let `show()` own the loop as above.
- Stop the host thread before `shutdown()` or the destruction of the application: no call into WebFront may still
  be running then.

## Concurrency and lifetime, in short

- Callbacks, handlers and task bodies run on the server thread, one at a time. They must not wait for that thread:
  no `future.get()` of a typed `jsFunction` call, no `shutdown()`, no `UIThread::stop()` inside them.
- `app`, a `UI` and a broadcast function may be used from host threads while the application lives; every such call
  must have returned before `shutdown()` or destruction starts.
- Start-up errors reach the host as exceptions: from the constructor (frontend initialization, listening socket),
  from `start()` / `show()` (the opening), or from `UIThread`'s constructor and `stop()`.
- `stop()` requests the server to stop; `shutdown()` and the destructor stop and join it and drain its tasks
  ([ADR-006](adr/ADR-006-taches-structurees-et-arret.md)). `requestClose()` ends the interface, after which `show()`
  stops and joins the server of an embedded frontend itself.

## Tests

`test/unit/FrontendContractTests.cpp` covers `start()` (launch on the calling thread, running server, failed launch,
a second call and a call mixed with `show()`), `requestClose()` across threads, the release of a close request in each
outcome of the executor, and `UIThread` (start, set-up and opening failures, a stop while the window is still
opening, a window that never closes, a window closed by the user) with mock frontends and no GUI. The shared lifecycle suite adds
`<Frontend>Lifecycle-host-close` (a host thread exchanges with the page, then closes the real window) and
`<Frontend>Lifecycle-host-thread` (`UIThread` with a real window where the frontend allows it; elsewhere the
main-thread diagnostic of a window opened on a secondary thread). CEF has no `host-thread` case.
