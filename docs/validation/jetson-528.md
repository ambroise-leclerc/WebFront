# Jetson Orin qualification (#528): JetPack 6, Xvfb

This is a partial qualification, not a Jetson installation guide. On 2026-11-12,
an Orin Nano Super 8 GB (L4T R36.4.7, Ubuntu 22.04.5, kernel 5.15.148-tegra,
X11 session) was tested at commit `a238090` with the GCC 13 build fix in
`test/web/WebKitGTKLifecycle.cpp`. The machine had GCC 13.4, CMake 3.31,
WebKitGTK development package 2.50.4, Xvfb and `xdg-desktop-portal-gnome` 42.1.
The distribution defaults were GCC 11.4 and CMake 3.22, below the project floors.

With GCC 13 selected as `CXX` and CMake 3.31 on `PATH`, the following fresh
build configured and compiled `webtest`, `webfront-wf`, `frontend_lifecycle`
and `WebFrontLifeReact`:

```sh
cmake -S . -B /tmp/webfront-528-webkit -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WEBKITGTK=ON \
  -DWEBFRONT_ENABLE_TESTING=ON -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK
cmake --build /tmp/webfront-528-webkit --target webtest webfront-wf WebFrontLifeReact --parallel 2
ctest --test-dir /tmp/webfront-528-webkit -R '^WebKitGTK(Adapter|Lifecycle)-' --output-on-failure --no-tests=error
ctest --test-dir /tmp/webfront-528-webkit -R '^WebFrontWebKitGTKIntegration$' --output-on-failure --no-tests=error
```

The adapter and lifecycle selection passed **28/28** tests; Jasmine and its
six fixture tests passed **7/7**. CTest uses Xvfb, X11 and software rendering
for these cases. The automated file chooser case passes, but this result alone
does not establish that a real desktop portal and parent-window export work.

A separate fresh configuration with CEF, WebKitGTK and tests disabled used
GCC 13 and CMake 3.31. Configuration took 18.4 s and a cold-cache build of
the `HelloWorld` target took 79.9 s. The build ran concurrently with the
WebKitGTK compilation, so these are observations under contention rather than
isolated benchmarks. Both builds used at most two compiler processes in total.

Still to qualify: the real screen and GPU under X11 and Wayland (including
WebGL, 2D canvas and the DMA-BUF workaround if needed), desktop portal and
parent-window export, the React example in full-screen kiosk use, and the same
pass on JetPack 7.2. No result here establishes support for those paths or for
other Orin models. The server-only remote-browser path also remains untested.
