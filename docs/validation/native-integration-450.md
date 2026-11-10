# Native integration of the frontends (#450) — platform validation

Epic #450 (ADR-020, step 5) was written on macOS: #462 (interface-thread executor) and #463 (native window). This page
records what was then validated on Windows and Linux on 2026-11-10, at `develop` 3af5848, and what is left to do on a
Linux desktop.

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

## Linux — partly validated

- CI at 3af5848: `desktop (linux-gcc-webkitgtk)` passes (Xvfb, X11), and the dispatch of `BuildAndTest.yml` on `develop`
  ([run 42776804599](https://github.com/ambroise-leclerc/WebFront/actions/runs/42776804599)) passes `web-integration`
  with GCC and Clang (CEF) and both sanitizer jobs. #463 had not been through the CEF jobs before.
- WSL 2, Ubuntu 26.10, GCC 15.3, GLib 2.90, no GTK, no WebKitGTK, no portal, no browser:
  `native_integration_manual browser` PASS with the real GLib loop. The page was opened by the Windows browser through an
  `xdg-open` shim, so only the loop and the server are Linux there.
- The WebKitGTK path of the program (`portalParent`, `GtkFileDialog`) only passed a GCC syntax check
  (`-DWEBFRONT_EMBED_WEBKITGTK`, no GTK installed): **it has never run**.

### Left to do on a Linux desktop

Needs `libwebkitgtk-6.0-dev`, GTK 4.10 or later, and `xdg-desktop-portal` with a backend for the portal dialog.

```bash
cmake -S . -B build-450 -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_EMBED_CEF=OFF \
  -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build-450 --target native_integration_manual frontend_lifecycle --parallel 4
ctest --test-dir build-450 -R "WebKitGTKLifecycle-(ui-executor|native-window|second-open)$" --output-on-failure --no-tests=error
build-450/test/web/native_integration_manual browser
build-450/test/web/native_integration_manual embedded 15000
```

- [ ] `browser` with a Linux system browser (real `xdg-open`).
- [ ] `embedded` in a Wayland session: `portalParent` is `wayland:<handle>` (the export of
      `gdk_wayland_toplevel_export_handle` has not been tried on a real desktop), and the four `[look]` points of the dialog.
- [ ] `embedded` in an X11 session (or `GDK_BACKEND=x11` under XWayland): `portalParent` is `x11:<hex XID>`, same `[look]` points.
- [ ] Optional: WSLg (`docs/WEBKITGTK.md` asks for a manual check there).
- [ ] Record the results here and update the manual-check paragraph of `docs/FRONTENDS.md`.

If the program itself is wrong on that path (it could not be run), fix it in the same change.

## Not covered

- macOS: the epic was developed and tested there (WKWebView); `native_integration_manual` runs the common checks but opens
  no dialog on macOS.
- CEF: `embedded` reports the `CefBrowser*` and opens no parented dialog; the window is closed by hand.
- The parented dialogs of the future ADR-020 native backend: the check of `docs/FRONTENDS.md` is to be repeated with it.
