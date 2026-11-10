# Native integration of the frontends (#450) — platform validation

Epic #450 (ADR-020, step 5) was written on macOS: #462 (interface-thread executor) and #463 (native window). This page
records what was validated on Windows and Linux on 2026-11-10, at `develop` 3af5848 and on this branch (the Linux
runs on a real desktop).

## The manual program

`test/web/NativeIntegrationManual.cpp` (target `native_integration_manual`, never built or run by CTest/CI) drives the two
APIs with a real browser or window and a real native dialog:

```bash
cmake --build <build> --target native_integration_manual
<build>/test/web/native_integration_manual browser  [dialog-ms]   # BrowserWithUILoopFrontend
<build>/test/web/native_integration_manual embedded [dialog-ms]   # the build's WEBFRONT_WEBTEST_FRONTEND
```

`embedded` exists in a build configured with tests and an enabled embedded frontend (the one `frontend_lifecycle` uses).
The dialog closes by itself after `dialog-ms` (1500 by default). `[ok]`/`[FAIL]` lines are checked by the program and give
the exit status; `[look]` lines are what only a person can confirm.

Both modes check: executor unavailable and operation refused before `show()` and after the loop; operations on the thread
that called `show()`, in scheduling order; `app.nativeWindow()` and `ui.nativeWindow()` before, during (interface thread
and worker thread) and after. Then, per platform:

| Platform | Dialog | Checked by the program |
| --- | --- | --- |
| Windows | `IFileOpenDialog` (COM STA, modal loop), closed with `WM_CLOSE` | an operation runs while the dialog is modal; with a window: the dialog is owned by the `HWND`, the window is disabled meanwhile, enabled and active again afterwards |
| Linux, `browser` | none (no toolkit) | the operation runs inside the GLib default context; `libgio-2.0.so.0` loads |
| Linux, `embedded` (WebKitGTK) | `GtkFileDialog` parented to the `GtkWindow*`, cancelled through the executor | `portalParent` is `x11:<XID>` or `wayland:<handle>` within 7 s; the dialog completes on the interface thread; `[look]`: over the window, on top, blocking, focus returned |

## Windows — validated

Windows 11 Pro 22631, MSVC 14.51 (Build Tools 18), CMake 4.3.4, Ninja, Release, `-DWEBFRONT_EMBED_WEBVIEW2=ON
-DWEBFRONT_WEBTEST_FRONTEND=WebView2`.

- The whole tree but one file compiles with `/analyze` and `/WX`, the WebView2 adapter, `UIExecutor.hpp`, `UILoop.hpp` and
  `NativeWindow.hpp` included. `test/unit/MessagesTests.cpp` (from #457, unrelated) fails with C6262 under this compiler (#512);
  the test executables were then built with `-DWEBFRONT_MSVC_ANALYZE=OFF`.
- `WebView2Lifecycle-ui-executor`, `-native-window` and `-second-open` pass, with the Catch2 scenarios of the executor, the
  browser loop and the native window.
- `native_integration_manual browser`: PASS. The system browser connected, `ui.nativeWindow()` is `notEmbedded`, the
  `IFileOpenDialog` opened without an owner from an operation of the executor, and an operation scheduled while it was
  modal ran: the message-only window of the loop is dispatched by the dialog's own loop.
- `native_integration_manual embedded` (WebView2): PASS. `app.nativeWindow()` and `ui.nativeWindow()` give the same
  top-level `HWND` of the interface thread, `wrongThread` from a worker and `closed` after the loop; the dialog is owned by
  that window, which is disabled while the dialog is open, then enabled and active again. This is the manual check of a
  parented dialog of `docs/FRONTENDS.md` for Windows, with a dialog opened by the program instead of the native backend.
- Complete CTest run (`--parallel 4`, on this branch): 677 of 678 pass. `TestOptionsConfiguration` fails, unrelated to this
  epic: its "old build tree" fixture returns before `project()`, which the Ninja generator selected by the machine's
  `CMAKE_GENERATOR=Ninja` refuses ("The detected version of Ninja () is less than the version of Ninja required").
  A first run, stopped at about 603 of 678 for lack of memory, had also failed `JourneyRestoreSession`
  (`rename: Access is denied` while restoring a package); it passed in the complete run.

## Linux — validated

- CI at 3af5848: `desktop (linux-gcc-webkitgtk)` passes (Xvfb, X11), and the dispatch of `BuildAndTest.yml` on `develop`
  ([run 42776804599](https://github.com/ambroise-leclerc/WebFront/actions/runs/42776804599)) passes `web-integration`
  with GCC and Clang (CEF) and both sanitizer jobs. #463 had not been through the CEF jobs before.
- WSL 2, Ubuntu 26.10, GCC 15.3, GLib 2.90, no GTK, no WebKitGTK: `native_integration_manual browser` PASS with the real
  GLib loop and the page opened by the Windows browser through an `xdg-open` shim.
- Linux desktop, 2026-11-10, this branch: Wayland session (GNOME, `xdg-desktop-portal` with the gnome and gtk backends),
  GCC, GTK 4.22.4, WebKitGTK 2.52.6, Release, configured with the commands below.
  - `WebKitGTKLifecycle-ui-executor`, `-native-window` and `-second-open` pass.
  - `browser` with the real `xdg-open`: PASS.
  - `embedded 15000` under XWayland (`GDK_BACKEND=x11`): PASS, `portalParent` is `x11:1a00004`.
  - `embedded 15000` under native Wayland (`GDK_BACKEND=wayland`): PASS, `portalParent` is `wayland:<handle>`: the
    `gdk_wayland_toplevel_export_handle` export works on a real desktop. The parented `GtkFileDialog` opened from an operation
    of the executor, an operation ran while it was open, it completed on the interface thread and `gtk_window_is_active` was
    true afterwards.
  - No program fix was needed on the WebKitGTK path.
  - Complete CTest run on this branch (`--parallel 4`, Xvfb, GCC 16.1, Linux 7.0): the 126 `web-integration` cases and the
    other targets pass, including `WebKitGTKLifecycle-*`. The `tests` executable does not compile with the default
    `-Werror`: GCC 16.1 reports `-Wnull-dereference` in `basic_string.h` for `StorageTests.cpp` and `NativeStorageTests.cpp`,
    which this branch does not touch. Built with `-DWARNINGS_AS_ERRORS=OFF`, 5 of 537 `fast` scenarios fail, all in
    `NativeStorageTests.cpp` and about symbolic links (`linkRefused` expected, another error code returned, for example
    `8 == 7` in "A link followed inside the root never reaches a temporary file"). Both are outside this epic and are
    to be tracked separately.
  - The `[look]` points (over the window, on top, blocking, focus returned) were confirmed visually by a person under native
    Wayland (`embedded 20000`); they were not looked at under XWayland.

### Commands

Needs `libwebkitgtk-6.0-dev`, GTK 4.10 or later, and `xdg-desktop-portal` with a backend for the portal dialog.

```bash
cmake -S . -B build-450 -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_EMBED_CEF=OFF \
  -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build-450 --target native_integration_manual frontend_lifecycle --parallel 4
ctest --test-dir build-450 -R "WebKitGTKLifecycle-(ui-executor|native-window|second-open)$" --output-on-failure --no-tests=error
build-450/test/web/native_integration_manual browser
build-450/test/web/native_integration_manual embedded 15000
```

Not done: WSLg (`docs/WEBKITGTK.md` asks for a manual check there).

## Not covered

- macOS: the epic was developed and tested there (WKWebView); `native_integration_manual` runs the common checks but opens
  no dialog on macOS.
- CEF: `embedded` reports the `CefBrowser*` and opens no parented dialog; the window is closed by hand.
- The parented dialogs of the future ADR-020 native backend: the check of `docs/FRONTENDS.md` is to be repeated with it.
