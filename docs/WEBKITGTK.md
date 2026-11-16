# WebKitGTK on Linux

WebKitGTKFrontend is an optional GTK 4 embedded window using the distribution's
WebKitGTK 6.0 runtime. It never changes the system-browser default.
For NVIDIA Jetson Orin, see the [Jetson qualification guide](JETSON.md), including
the separate real-display Canvas/WebGL probe; Xvfb software rendering does not
qualify the Tegra GPU.

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
uses `GSK_RENDERER=cairo` and `LIBGL_ALWAYS_SOFTWARE=1`.

Adapter-specific CTest names use `WebKitGTKAdapter-<case>`; the
`WebKitGTKLifecycle-<case>` names identify the shared lifecycle suite. Toolkit-free
unit tests cover the policy decision table, null error diagnostics, preserving
the first failure when a timeout or callback reports a later error, and the
sandbox launcher check described below.

## Sandbox and user namespaces

WebKit starts its web process through bubblewrap (`/usr/bin/bwrap`), which needs an
unprivileged user namespace. Some systems refuse one: Ubuntu 24.04 restricts user
namespaces through AppArmor and ships no profile for `bwrap`, and restricted
containers may reject the namespace setup. WebKit then aborts the whole process
before any navigation (`bwrap: setting up uid map: Permission denied`, exit 133).

`WebKitGTKFrontend::initialize()` therefore tries the launcher once per process
before creating the window (`bwrap --ro-bind / / bwrap --version`). When
bubblewrap reports that it cannot set up its namespace, WebFront sets WebKit's own
switch, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`, for this process and prints
once on standard error:

```text
WebFront: this system refuses WebKitGTK's web process sandbox (bwrap: setting up uid map: Permission denied); the window opens without it. Display only content you trust (docs/WEBKITGTK.md, "Sandbox and user namespaces").
```

**An application therefore builds and opens its native window on such a system
with no change to the operating system.** This is the only case in which the
library sets that variable: where bubblewrap works, the sandbox is kept, and the
library never changes system policy.

What this means: the web process then runs with the rights of the application,
like the application's own C++ code. The page of a WebFront application is its own
interface, served by its own process, and the content policy keeps top-level
navigation on that origin; that policy does not cover HTTP(S) iframes and
subresources. An application that displays content it does not control should not
rely on this mode:

- set `WEBFRONT_REQUIRE_WEBKIT_SANDBOX=1` in its environment: a refused sandbox is
  then an initialization error (`std::runtime_error` carrying bubblewrap's
  message), which `frontend::NativeOrBrowserFrontend` turns into the system
  browser;
- or let the administrator allow bubblewrap. Ubuntu 25.04 and later load a
  `bwrap-userns-restrict` AppArmor profile that lets bubblewrap set up a sandbox
  and strips capabilities from what runs inside it
  ([release notes](https://documentation.ubuntu.com/release-notes/25.04/#apparmor-profile-for-bwrap));
  Ubuntu 24.04 ships the same profile, disabled, in the `apparmor-profiles`
  package (`/usr/share/apparmor/extra-profiles/bwrap-userns-restrict`). The
  [Jetson guide](JETSON.md#ubuntu-2404-user-namespaces) records what was observed
  on such an installation.

The check is skipped where WebKit does not use bubblewrap (Flatpak, Snap, Docker
or Podman containers, or the variable already set), when `/usr/bin/bwrap` is
absent, and when bubblewrap fails for a reason other than its namespace; WebKit
then reports its own error.

On a trusted, isolated CI runner the variable can also be set by hand, as this
project's workflows do. Do not set it for browsing untrusted content.
