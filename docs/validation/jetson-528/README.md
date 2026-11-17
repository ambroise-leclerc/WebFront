# Jetson Orin qualification, 2026-11-16 (#528)

The measured board is an Orin Nano Super 8 GB running L4T R39.2.1, Ubuntu
24.04.5, GCC 13.3 and WebKitGTK 2.52.6 / GTK 4.14.5. The final contributor build
uses CMake 4.4.4, Release, CEF off and WebKitGTK on, with one compiler at a time.
The source baseline is `6ccb7e1` (merged #600); these changes add documentation
and measurement evidence, with no production-header change.

The `nvidia-jetpack` metapackage is absent. The reproducible identifier is the
L4T/kernel/package combination, not an inferred JetPack marketing version.
The existing workstation was not reflashed for this test.

## Complete automated baseline

The fresh `build-528-final` Release build uses CEF off, WebKitGTK on,
`WEBFRONT_WEBTEST_FRONTEND=WebKitGTK`, CUDA off and a one-slot Ninja compile
pool. Building all targets with `--parallel 2`, then running
`ctest --test-dir build-528-final --output-on-failure --parallel 2
--output-junit qualification-results.xml` completed in 526 seconds: 732 tests,
731 passed, zero failures, one skipped CUDA scenario because CUDA is disabled.
All 115 `web-integration` cases passed, including Jasmine and lifecycle tests
under Xvfb. [results.json](results.json) records the measured configuration.

An additional full Jasmine run used the real X11 desktop, the same `webtest`
binary and installed suite, without software-renderer or DMA-BUF overrides:
141 specs passed, zero failed or pending ([report](jasmine-real-x11.json)).
The real-desktop `frontend_lifecycle` modes `ui-executor`, `native-window`,
`second-open` and `host-close` each returned zero; their `lifecycle-x11-*.log`
files retain stderr. The full lifecycle matrix above remains the Xvfb result.

## Real desktop observations

| Check | Observed result | Evidence |
| --- | --- | --- |
| X11 Canvas 2D, WebGL pixel readback and WebKit capture | 3/3 Jasmine specs passed; red and green squares also present on the physical desktop capture | [report](graphics-x11.json), [native window](graphics-x11.png) |
| EGL driver during an extra graphics pass | NVIDIA Corporation, NVIDIA Tegra Orin (nvgpu)/integrated, OpenGL ES 3.2 NVIDIA 595.78 | [context log](egl-contexts.log), [diagnostic source](egl-audit.c) |
| X11 portal export and cancellation | `x11:3c00004`; the real GNOME file chooser was modal and transient for that window; callback completed on the UI thread and focus returned | [probe log](portal-x11.log) |
| X11 real file selection | The chooser accepted the explicit test file `/tmp/webfront-528-portal-selected.txt`; probe returned `RESULT: PASS` | [selection log](portal-x11-choose.log) |
| Life React native window and C++ panel | A new project copied from `examples/life/react` installed from the local registry; `wf dev main.tsx` rendered the grid and both generation labels. Clicking Next generation changed both labels from 0 to 1 | [native fullscreen capture](life-react-fullscreen.png) |
| Fullscreen Life React | `_NET_WM_STATE_FULLSCREEN`; window origin `(0,0)`, size `2160x3840`, matching the portrait desktop | [capture](life-react-fullscreen.png) |
| Wayland client under Weston 13, nested on X11 | 3/3 graphics specs passed, with `DISPLAY` unset and `GDK_BACKEND=wayland`; the actual outer compositor window showed both squares | [report](graphics-wayland-nested.json), [desktop capture](graphics-wayland-nested.png) |
| Nested Wayland compositor driver | Weston reported EGL/GL NVIDIA and the Tegra Orin renderer | [Weston log](weston.log) |
| Portal parent on this nested compositor | No exported Wayland parent identifier; the probe reports exactly one failed assertion. This path is not qualified | [probe log](portal-wayland-nested.log) |

Every graphics pass unsets `GSK_RENDERER`, `LIBGL_ALWAYS_SOFTWARE` and
`WEBKIT_DISABLE_DMABUF_RENDERER`. No DMA-BUF opt-out was needed for the observed
default rendering path. This is not a claim that every buffer used DMA-BUF or
that other driver versions require no workaround.

WebKit returns a constant `Apple GPU` to the WebGL debug-renderer query, including
on Linux. That label is visible in the captures and is not the hardware identity.
The [WebKit implementation](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/html/canvas/WebGLRenderingContextBase.cpp)
returns this constant for `UNMASKED_RENDERER_WEBGL`. The separate EGL measurement
checks the native context after a successful `eglMakeCurrent`, in addition to the
uninstrumented pixel/presentation test. `glxinfo -B` independently reports direct
rendering with NVIDIA Tegra Orin / NVIDIA 595.78.

The extra diagnostic can be reproduced without installing it into the application:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -shared -fPIC -O2 \
  docs/validation/jetson-528/egl-audit.c -o /tmp/webfront-528-egl-audit.so
```

Repeat the graphics recipe of [JETSON.md](../../JETSON.md#real-display-graphics-probe)
with `LD_AUDIT=/tmp/webfront-528-egl-audit.so` added to that invocation's environment,
then inspect the `WF528_CONTEXT` lines. The normal run has no audit library.

## Life React reproduction

Use the examples build and registry preparation from [JETSON.md](../../JETSON.md#build-examples-first).
The observed pass used a fresh project, the local TestRegistry mirror and the
repository's existing `webfront-wf` to install packages before opening the native
application. Its manifest contained `"registry": "/registry/"`. The application
was then started as:

```sh
./build-528-final/examples/WebFrontLifeReact /tmp/webfront-528-life-project
```

In its developer terminal, `wf dev main.tsx` opened the preview. Closing the
terminal overlay leaves the canvas and C++ panel interactive. The window manager
was asked to make this one window fullscreen with `wmctrl -i -r <window-id> -b
add,fullscreen`; `xprop` and `xdotool getwindowgeometry` confirmed the state and
size. This does not introduce a fullscreen API or lock the desktop into kiosk mode.

## Wayland scope

`/etc/gdm3/custom.conf` contains the active setting `WaylandEnable=false`; the
owner's active session is X11. Neither that setting nor the active login session
was changed. A native GNOME Wayland session is therefore not qualified here.

The installed NVIDIA Weston 13 package lacks an X11 backend. Ubuntu's `weston`
and `libweston-13-0` 13.0.0-4build3 packages were downloaded and extracted under
`/tmp`, without installation or replacing the NVIDIA packages. The extracted
Weston used its X11 backend, GL renderer and kiosk shell (which supports GTK's
xdg-shell clients), with a dedicated `wayland-webfront-528-kiosk` socket. Its
module paths and exact invocation are recorded in [weston.log](weston.log).

The client used `GDK_BACKEND=wayland`, that socket, and no `DISPLAY`; the server's
outer X11 window supplied the desktop presentation evidence. This demonstrates
the Wayland client path, not DRM scan-out or GNOME Wayland portal integration.
The empty parent identifier is retained as a limitation, not converted into a
successful portal result.

## Coverage boundary

The tested profile is Nano Super 8 GB / the exact L4T stack above / X11, at S2.
JetPack 6 remains a historical S3 path with the separate results in JETSON.md.
NX and AGX share the aarch64/L4T software requirements; no physical NX/AGX display
or GPU qualification is inferred. Fresh-flash acceptance, a locked-down kiosk
session and native GNOME Wayland remain separate observations. The supported
native X11 example is demonstrated from a fresh application project here.
