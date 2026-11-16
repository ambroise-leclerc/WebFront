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

## Development and distribution

WebKitGTK has two **build modes**, independent of Debug/Release, `NDEBUG`,
`wf embed`, and whether the application calls `bootstrap("developer")`:

| Mode | Selection | Web process |
| --- | --- | --- |
| Development (default) | `WEBFRONT_DISTRIBUTION=OFF` | Sandbox explicitly disabled; native windows and Jasmine run without configuring the OS |
| Distribution | `WEBFRONT_DISTRIBUTION=ON` or `webfront_use_distribution(target)` | Sandbox required; a conflicting environment or refused namespace is an initialization error |

The CMake configuration announces the default mode. In development, initialization
prints once per process:

```text
WebFront: WebKitGTK development mode: sandbox disabled. Use WEBFRONT_DISTRIBUTION=ON for packaged applications (docs/WEBKITGTK.md).
```

This mode sets `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` in the application's
process before GTK initialization, on every system, without probing bubblewrap.
It is for trusted development interfaces and specs. The web process has the
application's OS rights; the origin policy does not isolate HTTP(S) iframes,
subresources or a vulnerable renderer. A native Jasmine pass in development
therefore validates the page, bridge and lifecycle, not renderer isolation.
Neither mode changes AppArmor, sysctls or installed packages at run time.

### Distribution

Select the policy in the build, rather than relying on the launching environment:

```bash
cmake -S . -B build-distribution -DWEBFRONT_EMBED_WEBKITGTK=ON \
  -DWEBFRONT_DISTRIBUTION=ON -DCMAKE_BUILD_TYPE=Release
```

For a consumer with separate development and packaged executables, call
`webfront_use_distribution(my_app)` after creating the packaged target. The helper
adds `WEBFRONT_DISTRIBUTION=1` to that executable only. Header-only consumers
without CMake can define the same macro consistently in all their translation
units. This switch currently controls the WebKitGTK sandbox policy; it does not
embed assets, remove developer callbacks or generate an installer.

An inherited `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` is rejected, including
inside containers. Unset the variable or set it to `0`, the upstream value that
keeps the sandbox enabled. Environment variables cannot downgrade a distribution
build. In a development build, `WEBFRONT_REQUIRE_WEBKIT_SANDBOX=1` selects the
same strict policy; `0` does not. A refusal throws before WebKit can abort.
`NativeOrBrowserFrontend` may then use its documented system-browser fallback;
applications requiring the embedded window select `WebKitGTKFrontend` directly.

### Sandbox and user namespaces

On ordinary Linux hosts, WebKit uses bubblewrap and unprivileged user namespaces.
Distribution initialization checks the launcher before opening the window. Some
Ubuntu 24.04 installations lack the AppArmor integration that allows this setup;
Ubuntu introduced a `bwrap-userns-restrict` profile in later releases
([release notes](https://documentation.ubuntu.com/release-notes/25.04/#apparmor-profile-for-bwrap)).
The check is skipped where WebKit uses its container integration (Flatpak, Snap,
Docker/Podman markers), when the launcher is absent, or when its failure is
unrelated to namespaces; WebKit retains responsibility for those launch paths.
A successful probe is not an attestation of WebKit's complete sandbox.

### Package the application's profile

A distribution executable can ship its own AppArmor namespace authorization.
WebFront generates a profile for **one literal installed executable**, without
adding a runtime or changing the development machine:

```cmake
add_subdirectory(WebFront)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE WebFront::WebFront)
webfront_use_native_window(my_app)
install(TARGETS my_app RUNTIME DESTINATION bin)
if(WEBFRONT_DISTRIBUTION)
    webfront_generate_webkitgtk_profile(
        TARGET my_app
        EXECUTABLE /usr/bin/my_app
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/apparmor/webfront.my_app")
endif()
```

Generating a profile does not change the target's build mode; the project stays
in development by default even when preparing packaging files. Select distribution
with the option or `webfront_use_distribution(my_app)`. The installed path must match the package, including the executable's final name; it must be
stable and controlled by the installer. Patterns are rejected. Profile identifiers
include a hash of that path so unrelated packages with a target called `app` do
not replace each other's policies. It grants `userns` to the application to let
WebKit establish its own sandbox; it is not a confinement profile for the C++ host.

The generated template uses AppArmor ABI 4.0, as qualified on Ubuntu 24.04.
For an Ubuntu 24.04+ application package, add `dh-apparmor` to its build dependencies,
ship the generated file in `/etc/apparmor.d/`, and let the packaging machinery
reload it on installation. For example, with debhelper configured to build in
`build/`, `debian/my-app.install` includes:

```text
build/apparmor/webfront.my_app etc/apparmor.d
```

In `debian/rules`, after `dh_install` in `override_dh_install`, run:

```make
	dh_apparmor --profile-name=webfront.my_app -pmy-app
```

Configure that package with `-DCMAKE_INSTALL_PREFIX=/usr` and
`-DWEBFRONT_DISTRIBUTION=ON`. Declare the WebKitGTK runtime dependency explicitly
(`libwebkitgtk-6.0-4` on Ubuntu); `dlopen` dependencies are not discovered by
ordinary ELF dependency scanning. The profile-installation recipe also needs the
AppArmor loader supplied by `apparmor`. For older distributions, adapt the
profile to their supported ABI/policy rather than installing this template
unconditionally. The packaging hook is required: copying the file
alone does not activate a profile. Use the normal package manager to install and
remove the application; no manual AppArmor editing is part of the end-user flow.
See [`dh_apparmor`](https://manpages.debian.org/trixie/dh-apparmor/dh_apparmor.1.en.html).
The library itself still has no install/export rules; these are application
packaging steps.

The installed profile does not cover arbitrary executables under `build/`.
Development windows and Jasmine use development mode without installation.
Before shipping, additionally test the installed distribution application with
its sandbox active and without the upstream opt-out. Generated-profile syntax
and staging tests do not replace that on-system qualification; the generated
profile was validated by installing and removing a temporary native application
package on the Jetson: its bridge connected, its renderer had a separate mount
and PID namespace, no effective capabilities, `NoNewPrivs=1` and seccomp active.
See [the qualification record](JETSON.md#hybrid-policy-qualification-597). This
software-rendered Xvfb check does not qualify accelerated rendering or every
application package.
