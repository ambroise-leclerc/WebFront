# WebKitGTK on Linux

WebKitGTKFrontend is an optional GTK 4 embedded window using the distribution's
WebKitGTK 6.0 runtime. It never changes the system-browser default.

```cpp
#include <webfront/WebFront.hpp>
using App = webfront::WebFrontWithFrontend<webfront::frontend::WebKitGTKFrontend>;
int main() {
    App app;
    app.show();
}
```

Install development packages before configuring:

| Distribution | Packages |
| --- | --- |
| Ubuntu 24.04 / 26.04, Debian with WebKitGTK 6.0 | `sudo apt install libwebkitgtk-6.0-dev pkg-config xvfb` |
| Fedora | `sudo dnf install webkitgtk6.0-devel pkgconf-pkg-config xorg-x11-server-Xvfb` |
| Arch Linux | `sudo pacman -S webkitgtk-6.0 pkgconf xorg-server-xvfb` |

```bash
cmake -S . -B build-webkitgtk -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_ENABLE_TESTING=ON \
  -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK
cmake --build build-webkitgtk --parallel 2
ctest --test-dir build-webkitgtk -L web-integration --output-on-failure --no-tests=error
```

CMake discovers `webkitgtk-6.0` with pkg-config and exposes `PkgConfig::WebKitGTK`.
The development package is required only for this configure-time detection. The
adapter neither includes its SDK headers nor links the imported target; a deployed
executable needs only the WebKitGTK 6.0 runtime and its runtime dependencies, not
the development package or pkg-config.
GTK 3 / WebKitGTK 4.1 and the obsolete libsoup 2 / WebKitGTK 4.0 API are not supported.
The header-only adapter loads `libwebkitgtk-6.0.so.4` locally with `dlopen`; no GTK,
GLib or X11 headers or macros propagate through `WebFront.hpp`. Disabled builds
need neither pkg-config nor WebKitGTK. A missing runtime reports an exception.
Embedded Jasmine, React and Babel assets use gzip because WebKitGTK does not
advertise Brotli over local plain HTTP.
The module remains loaded for process-global GTK type registrations; each opening
releases its window, web view, network session, handlers and main loop.

Construct the application and call `show()` on the **process main thread**. Both
initialization and opening reject worker threads before touching GTK. `show()` runs
the GTK loop while WebFront serves requests on its own thread. Closing the native
window or calling `window.close()` returns even if no bridge connected. Unreachable
initial navigation and HTTP errors throw; initial navigation has a 30-second bound
until document commit, independent of bridge readiness and slow page subresources.
Do not nest or overlap GTK sessions, or combine this loop with another toolkit's
active main loop. CEF may be enabled in the same binary, but its window loop and
GTK's must be used separately on the main thread.

A working X11 (`DISPLAY`) or Wayland (`WAYLAND_DISPLAY`) desktop is required. Failed
`gtk_init_check()` throws an error suggesting the display variables and Xvfb.
On WSLg, preserve its supplied display variables and runtime directory. Native
Wayland and WSLg require manual verification on those desktops; automated coverage
uses X11/Xvfb. No WSL-specific library environment changes are applied.

Persistent data lives in `$XDG_DATA_HOME/webfront/<executable>-<path-hash>` and caches
in `$XDG_CACHE_HOME/webfront/<executable>-<path-hash>`. Absolute XDG paths are honored;
otherwise absolute `$HOME/.local/share` and `$HOME/.cache` are used. The stable hash
of `/proc/self/exe` separates equally named executables in different directories.
The per-executable data and cache directories are restricted to mode `0700`,
including existing profiles, independently of the process umask.
Directory creation errors propagate instead of silently using a shared temporary
profile. Instances within a process and concurrent processes launched from the
same executable path share these persistent directories; there is no per-instance
profile isolation or per-application profile option. To isolate separate processes,
launch each with different absolute XDG roots. CTest explicitly appends private
data/cache paths inside its build tree to every WebKitGTK test environment.

The shared content policy blocks unmanaged popups, controls DevTools and context
menus, and checks main-document HTTP(S) responses before commit, including redirected
destinations. WebKitGTK's navigation-action API does not identify unnamed frames,
so origin checks happen at the response decision: a denied HTTP request may already
have reached its destination, while its document is never committed. HTTP(S)
iframes and subresources remain outside the top-level origin policy.
The navigation-action scheme filter applies to every frame: `data:`, `blob:` and
`file:` are rejected, including in iframes; `about:blank` and `about:srcdoc` are
allowed. Bridge origins are checked separately by the server. Popup forwarding
requires a real user gesture.

CTest runs Jasmine and all shared lifecycle cases under Xvfb, plus missing-display,
worker-thread, repeated-show, HTTP-error, pre-run failure and profile-permission
cases. Native-close schedules a GTK timeout, checks that the loop is running and
then closes the real window. Errors recorded before `run()` are also propagated. Software rendering
uses `GSK_RENDERER=cairo` and `LIBGL_ALWAYS_SOFTWARE=1`. WebKit's sandbox uses bubblewrap
and user namespaces. Restricted containers may reject its namespace setup. For a
trusted, isolated CI runner only, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` is an
upstream diagnostic escape hatch; the library never sets it. Prefer enabling user
namespaces. Do not use that escape hatch for browsing untrusted content.

Adapter-specific CTest names use `WebKitGTKAdapter-<case>`; the
`WebKitGTKLifecycle-<case>` names identify the shared lifecycle suite. Toolkit-free
Catch2 tests cover the policy decision table, null error diagnostics and preserving
the first failure when a timeout or callback reports a later error.
