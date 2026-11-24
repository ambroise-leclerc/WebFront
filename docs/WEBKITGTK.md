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

### Snap-hosted editor terminals

A terminal inherited from a Snap-packaged editor exposes Snap library and GTK
module paths. This is a normal way to start an application, so WebFront repairs it.
The cause is isolated: `GIO_MODULE_DIR` alone is enough. With VSCodium the Snap's
GIO modules (`libgiognutls.so`, `libgiolibproxy.so`) carry an RPATH to
`/snap/core20/current/lib`; the system `WebKitNetworkProcess` loads them and then
Snap's `libpthread.so.0`, which fails with a `GLIBC_PRIVATE` symbol error
(`undefined symbol: __libc_pthread_init`). Any GIO program does the same, not only
WebFront.

When `/snap/` or `/var/lib/snapd/snap/` paths appear outside a verified Snap
confinement in `LD_LIBRARY_PATH`, `LD_PRELOAD`, `GIO_MODULE_DIR`, `GTK_PATH`,
`GTK_EXE_PREFIX`, `GTK_IM_MODULE_FILE`, `GSETTINGS_SCHEMA_DIR`, `LOCPATH`,
`XDG_DATA_DIRS` or `XDG_CONFIG_DIRS`, the frontend repairs them before the toolkit
starts, in the application's own environment, which the processes WebKit launches
inherit, and logs the names it changed at `warn`:

- the value saved by VS Code and VSCodium in `<NAME>_VSCODE_SNAP_ORIG` is restored
  (an empty one means the variable is unset);
- otherwise the Snap entries are removed from a list, or a single Snap path is unset.

Values that name no Snap path, including `/home/<user>/snap/...`, are left alone, and
nothing changes inside a verified Snap confinement. Other variables are not yet
covered; report a failure with its variable. A launcher that needs a Snap value
there must set it again after `show()` or `start()` returns.

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

## Startup and first display

The window is mapped at once, but almost transparent, and becomes opaque when the page reports its first frame (two
animation frames after `DOMContentLoaded`). If no frame is reported, a one-second timer shows it anyway, even when the
server has sent nothing yet, so a window may still appear empty (in the colour of `background`, if any) when the load is
slow; GLib can also run the timer late when other events keep the main loop busy. Without a compositing manager the
opacity is ignored and the window appears immediately, as before. The navigation timeout and its diagnostics are
unchanged. `WindowOptions::background` (`"#rgb"`, `"#rrggbb"` or `"#rrggbbaa"`) is the colour of the view under the page,
so a dark interface does not flash white once the window is shown; only WebKitGTK applies it so far.
`WebKitGTKStartupTime-cold` and `-warm` (label `startup`, in the standard tier) start the lifecycle program's `startup-time`
scenario with the trace enabled and fail when the page needs more than 10 s (cold: fresh `HOME` and `XDG_*` directories,
hence an empty GStreamer registry and font cache) or 5 s (warm: the same directories after a first start) to answer the host.
The time runs from the start of the program, and each case also requires the window's `window shown` milestone (the close waits 1.5 s for it) and applies the limit to it. The limits only catch a regression of seconds on a loaded runner; they do not qualify the 500 ms and 1 s targets.
`WEBFRONT_STARTUP_TRACE=1` prints the view's milestones (session creation, view created, load started, first frame,
window shown) on standard error, in milliseconds since the process was executed; only the first frame notification of
the session is a milestone (a late one is marked as such), a reload is not. A close request (`requestClose()`) made
before the window is shown still ends the session.

### Skipping GStreamer

WebKitGTK initializes GStreamer in the page's process shortly after a page loads, even if it never uses media, and scans
every plugin when the registry is cold (the first launch of a user, and after each update of the system's plugins): up to
1.7 s during which the WebSocket handshake and the frames of the page are blocked. Disabling the media settings of the
view does not prevent it, and WebFront cannot know in advance whether a page will use media. An application whose pages
never use `<audio>`, `<video>` or Web Audio therefore opts out with `WebFrontConfig::window.media = false`: WebKit's
processes are launched with an empty `GST_PLUGIN_SYSTEM_PATH_1_0` and `GST_PLUGIN_PATH_1_0` and a registry of the
application's profile (`no-media-registry.bin`, so that the user's own registry is never rewritten). The variables are set from the creation of the view, which makes
WebKit launch a spare web process, until the page reports that its load started (or the one-second delay expires), then
restored. WebKitGTK offers no per-process environment, so **the variables are process-wide during that window**, a few
hundred milliseconds at the start of the session: a thread that initializes GStreamer in the application during it
reads the empty plugin directory and keeps that configuration after the restoration, and a process the application
starts during it inherits the variables. An application that starts GStreamer or child processes from other threads
at startup must keep `media = true` or wait for its page before doing so. A web process launched later in the session
(a process swap) runs without the restriction: it initializes GStreamer as usual, only slower. **A page that does use media
after the opt-out terminates its web process** (WebKitGTK cannot create its media elements without the plugins, and the
session fails with "web process terminated"), which is why the default is `true`. Other frontends ignore the option.

The cost of the first layout depends on the fonts of the page: fontconfig walks every installed font for each family it
cannot find or that is a CSS generic such as `system-ui` or `ui-monospace`, for each weight and style. A page that
lists several absent families (`"Segoe UI"`, `Inter`, `"JetBrains Mono"`, `Menlo`) can spend hundreds of milliseconds
before its first paint on a machine with thousands of fonts (hundreds of milliseconds measured on Ubuntu 26.04 with
WebKitGTK 2.52 and 2 333 installed fonts, for the stacks of one application). Even a generic or an installed family is
looked up once per variant, so a short stack does not make the first family free: it avoids the lookups of every family
that follows it. Keep stacks to a generic (`sans-serif`, `monospace`) or to a family you know to be installed, with no
absent families after it; the embedded pages, the starter and the examples do.

When an application serves a document root (`NativeDebugFS`, the development path), the first time it serves a `.css`
file whose `font-family` declaration lists four or more families other than `serif`, `sans-serif`, `monospace`, `cursive`
and `fantasy`, it writes one hint per file on standard error. The check (`include/webfront/system/FontStacks.hpp`) only
counts families, because it cannot know which are installed: it ignores comments, `var()` and custom properties, reads
nothing over 1 MiB, checks a file again when its size or modification time changes (and not otherwise) and never fails a request. A production build serves embedded files and does not run it. `wf check`
does not report font stacks: its checks type-check TypeScript, and a new check would change the environment's published
archive.

### Critical path and prewarming (analysis)

This is reasoning from the code and the traces of the issue, not a measurement; the reference machine has not been
used to qualify it. The sequence of a start is: `Frontend::initialize()` (toolkit and GPU initialization, about 90 ms in
the issue's trace), then the server's bind and listen in the constructor of the application, then at `show()` the
network session, the context (which launches a spare web process), the view and the load. The server answers no
request before `show()` starts its thread, and the load is requested only when the view exists, so listening before
`Frontend::initialize()` would move nothing on the path to the first request: it was not done. What can overlap is the
launch of the web and network processes with the application's own start, which the creation of the context at
construction would allow. It is not done because the context and the view take values known only at `show()` (the content
policy, the view's secret and origin, the window options, and `media`, which sets the environment of the processes at
creation); a context created earlier would have to be thrown away when any of them differs, and WebKitGTK has no
per-process environment (see above). A change would need the reference machine to show a gain on the first request
and a rule for what happens when the options differ, and the same question for WebView2 and WKWebView, whose web
content starts with the view.

## Development and distribution

WebKitGTK has two **build modes**, independent of Debug/Release, `NDEBUG`,
`wf embed`, and whether the application calls `bootstrap("developer")`:

| Mode | Selection | Web process |
| --- | --- | --- |
| Development (default) | `WEBFRONT_DISTRIBUTION=OFF` | Sandbox explicitly disabled; native windows and Jasmine run without configuring the OS |
| Distribution | `WEBFRONT_DISTRIBUTION=ON` or `webfront_use_distribution(target)` | Sandbox required; a conflicting environment or refused namespace is an initialization error |

For top-level builds, CMake announces the default policy; a per-target selection
can override it. Initialization reports the selected policy through the library's
logger (warning for development, information for distribution), so the application
can filter it or choose its sink.

Development temporarily sets `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` while
constructing an explicit WebKit context. WebKit snapshots this setting into its
process pool; WebFront restores the previous environment before creating the
window or calling application callbacks, including on failure. The system-browser
fallback therefore does not inherit an opt-out introduced by WebFront. This brief
operation still changes the process environment: applications must not concurrently
mutate it or launch unrelated children during native context construction.
It disables the native renderer sandbox on every system without probing bubblewrap.
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
`webfront_use_distribution(my_app native_ui)` after creating the packaged targets.
List every library that compiles native UI code: executable definitions are private;
library definitions propagate to their consumers. The helper does not traverse
linked dependencies or rebuild imported libraries. Prefer the global option when
all WebFront targets belong to the distribution. The selected frontends are distinct
C++ template specializations, so the linker cannot substitute development methods
for distribution methods from another translation unit. Attempting to initialize
both policies in one process is rejected. Header-only consumers
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
Flatpak delegates isolation to its package sandbox. Snap variables are accepted
only when `/proc/self/attr/current` confirms an enforced `snap.<name>.*` AppArmor
label matching `SNAP_NAME`; inherited variables alone are rejected in distribution.
These verified package environments skip this probe. Docker runs the bubblewrap
probe, including its proc/dev mounts and `--unshare-all` setup. Distribution
rejects environments with `/run/.containerenv` (including Podman): WebKit can
silently disable bubblewrap if its later container probe fails, even after a
successful WebFront preflight. WebKitGTK exposes no supported fail-closed hook
for that fallback. Development remains available there; distribution must use a
qualified environment without that marker. A missing launcher or any unsuccessful probe
rejects distribution initialization, even when stderr is empty. The container refusal prevents WebKit's fallback from silently disabling its sandbox.
The environment opt-out is checked again on every initialization call. The probe
has a three-second deadline; its supervisor owns the launcher child, so a host
that ignores `SIGCHLD` or reaps its own children does not invalidate its result.

CMake discovers `bwrap` on its search path. Set
`WEBFRONT_WEBKITGTK_BWRAP_EXECUTABLE` to **the path compiled into the target WebKit
engine** when it differs (for example, a Nix/Guix store path). Standalone headers
use `bwrap` on `PATH`, or accept the same quoted compile definition. This is a
preflight of that launcher, not an attestation of every WebKit build or its full
sandbox; nonstandard builds must be qualified with their actual engine.

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
        APPARMOR_ABI 4.0 # Declare the target OS policy ABI, not the build host ABI.
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

Generation requires an explicit `APPARMOR_ABI 4.0`, as qualified on Ubuntu 24.04.
Other values are rejected during configuration. This profile is intended for
targets with AppArmor 4 and restricted unprivileged user namespaces. Do not ship it
unconditionally on Ubuntu 22.04 or Debian 12: those older policy parsers do not
support this template, and their ordinary namespace setup does not need this
AppArmor 4 authorization. Build a separate package variant without the generated
profile and qualify the sandbox on that target.
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
AppArmor loader supplied by `apparmor`. For older distributions, omit this template as described above. The packaging hook is required: copying the file
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
