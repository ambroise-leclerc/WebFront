# Testing an existing WebFront application with Jasmine and CTest

`webfront_add_jasmine_test` instruments the application's actual page. Its document root,
ordinary scripts and C++ callbacks remain available to the specs. It uses the real Jasmine,
without the developer terminal, a wf manifest or a JavaScript runtime on the host.

An application must explicitly enable instrumentation in a test build. Before opening the page,
retain `webfront::testing::JasmineTest`; after the window closes, shut down and return its status:

```cpp
#ifdef WEBFRONT_JASMINE_TESTING
#include <webfront/testing/Jasmine.hpp>
#endif

// Construct app with an embedded frontend and port "0" to allow parallel tests.
// Register the application's ordinary callbacks before show().
#ifdef WEBFRONT_JASMINE_TESTING
webfront::testing::JasmineTest tests{app};
#endif
app.show();
app.shutdown();
#ifdef WEBFRONT_JASMINE_TESTING
return tests.exitCode();
#else
return 0;
#endif
```

In the consuming CMake project, after `enable_testing()`, `add_subdirectory(WebFront)` and
creating the application target:

```cmake
webfront_add_jasmine_test(NAME BoardUI TARGET SiliconForge
    SPECS tests/ui/board.spec.js ARGS firmware.bin FRONTEND WKWebView TIMEOUT 60)
```

The helper is available after adding WebFront; registration defines `WEBFRONT_JASMINE_TESTING`
on the specified target. Build the application and run `ctest --output-on-failure`. Without that
macro `JasmineTest` is an inert class that registers nothing and never reads
`WEBFRONT_JASMINE_CONFIG`, so a distribution build cannot be turned into a script loader through
the environment, even if it constructs the object unconditionally. Define the macro consistently
for every translation unit of one program.

`FRONTEND` validates that the named embedded engine is enabled for the current platform; the
application still selects its C++ frontend explicitly. CEF and WebKitGTK work headlessly under
Linux's `xvfb-run`, which must be installed. WKWebView and WebView2 require their native desktop
session. The system browser is unsuitable for these automated tests. If using the test driver's
frontend wrappers, they consume the isolated profile paths supplied by the runner. Other
applications must configure their frontend's persistent profile explicitly if it has one.

`ARGS` are passed literally as individual arguments, without a shell: empty strings, semicolons
and `$<` reach the application unchanged; an argument containing `]==]` is refused.
`TIMEOUT` defaults to 60 seconds (1–3600). The runner enforces it outside the application; CTest
has a slightly longer deadline. Therefore a synchronous infinite loop in a spec cannot report
success or hang CI indefinitely. On POSIX hosts the runner starts the application in its own
process group and kills the whole group at the deadline, so a renderer or `xvfb-run`'s X server
does not outlive it. On Windows only the direct child is stopped; check for a leftover renderer
after a timeout. Ports are not changed by the helper: use an OS-assigned port in the application.

`SPECS` are existing classic JavaScript files, loaded in order after the real page's load event
and bridge handshake. They may await application-specific readiness in `beforeAll`. Compile
TypeScript or module graphs into classic JavaScript beforehand; relative ES module imports and
TS compilation are not provided by this helper. Individual files are limited to 4 MiB, with at
most 100 spec files. A syntax error or top-level exception in any of them fails the run with a
`status: error` report naming the file; the other specs are not run on their own. The application needs its ordinary `/WebFront.js` and an embedded window
that permits the instrumentation script and closes on `window.close()`.

By default configure downloads and SHA-512 verifies `jasmine-core@7.0.2`, then extracts only
its browser runtime and license into the build directory. To configure offline, set
`WEBFRONT_JASMINE_ARCHIVE_DIR` to a directory holding the exact pinned
`jasmine-core-7.0.2.tgz`. `JASMINE /path/to/jasmine.js` instead uses a caller-provisioned runtime;
its provenance and version are the caller's responsibility. No public network is needed at test
execution. Jasmine stays outside the core's embedded resource list.

Each named test owns `webfront-jasmine/<name>/` in the calling binary directory: config, profile
and bounded JSON report. Names contain only letters, digits, underscore and hyphen. Do not run
two independent CTest processes for the same named test/build simultaneously. Use separate build
trees for those runs. A failed spec, suite/global error, focused suite, zero executed specs,
missing report, application failure or timeout fails CTest. Old reports are removed before each run.

The test build trusts the configured runtime/specs and application page. This is code execution
with application/bridge authority, not a sandbox. Native reads expose only the configured files
by numeric index, in bounded chunks, to the newest instrumented connection: a page that reloads
or redirects is instrumented again and the earlier connection loses access. One window is supported. No runtime path supplied
by a page is opened. Use an isolated CI environment for untrusted projects and remove secrets.

## Native programs and temporary files from the specs

A consumer's own tools and CLI tests (SiliconForge's `atdf2json`, a test that runs its `siliconforge` binary on a generated
description) need two things the browser lacks: running a program and a private place to write files. They are an explicit
**grant of the test's owner** (`PROGRAMS` in `webfront_add_jasmine_test`), never something a spec, a page or a manifest can ask
for:

```cmake
webfront_add_jasmine_test(NAME McuTools TARGET SiliconForge
    SPECS tests/ui/mcu-tools.spec.js
    PROGRAMS "$<TARGET_FILE:siliconforge>")     # absolute paths or generator expressions
```

Without `PROGRAMS` every `webFrontHost` call rejects with "No native capability was granted", and a program that is not listed
is refused with "not authorized": the path in the call must designate the same file as a listed one (a relative name, a shell
or another binary never matches). Existing tests and applications keep their behaviour; a build without
`WEBFRONT_JASMINE_TESTING` has no such code path.

```js
const [siliconforge] = await webFrontHost.programs();        // the authorized paths, in the order of PROGRAMS
await webFrontHost.withTemp(async dir => {                    // a new private directory, removed afterwards
    await webFrontHost.writeFile(`${dir}/chip.json`, JSON.stringify(description));
    const help = await webFrontHost.run({program: siliconforge, args: ['--mcu-path', dir, '--help']});
    expect(help.status).toBe('exited');                       // exited | signaled | timeout | cancelled | launchFailed
    expect(help.exitCode).toBe(0);
    expect(help.stdout).toContain('atmega328p');
    const bad = await webFrontHost.run({program: siliconforge, args: ['--mcu-file', `${dir}/chip.json`, 'blink.hex'],
                                        cwd: dir, timeoutMs: 20000});
    expect(bad.exitCode).toBe(1);
    expect(bad.stderr).toContain('interrupts[2]');
});
```

| Call | Meaning |
| --- | --- |
| `run({program, args, cwd, env, inheritEnv, timeoutMs, maxOutputBytes, signal})` | Runs the program to its end and resolves with `{status, exitCode, signal, error, stdout, stderr, stdoutTruncated, stderrTruncated, durationMs}`. Unknown options are an error. `signal` is an `AbortSignal`. |
| `programs()` | The authorized program paths. |
| `makeTemp()`, `removeTemp(dir)`, `withTemp(async dir => ...)` | Private directories under the test's `webfront-jasmine/<name>/temp/`. |
| `writeFile(path, textOrBytes)`, `readFile(path)`, `readText(path)` | Files inside those directories only (bytes cross the bridge in 48 KiB chunks; 64 MiB at most). |

What is guaranteed, and what is not:

- **No shell, literal arguments.** The program is started directly with `args` as its `argv`: spaces, quotes, Unicode, `;|&$\`` and
  empty strings arrive unchanged (Windows applies the standard `CommandLineToArgvW` quoting). Its standard input is closed.
  Paths may contain spaces and Unicode; the program path is the owner's, not the spec's spelling.
- **Environment policy.** By default the program sees **only** `env` (plus `SystemRoot` on Windows, which programs there need to
  start); `inheritEnv: true` adds the test process's environment under it. Neither changes `TMPDIR`: pass the variables you
  want, for example the directory from `makeTemp()`.
- **Working directory.** `cwd` must be inside a temporary space; the default is the test process's directory.
- **Results.** A non-zero exit, a signal (POSIX), a timeout (`timeoutMs`, 30 s by default, 1 ms to 1 h), a cancellation and a program
  that cannot start (`launchFailed`, with `error`) are results, not exceptions: assert on them. A refused request (unauthorized
  program, bad option, path outside the spaces, too many runs) rejects the Promise.
- **Bounded output.** Each stream keeps its first `maxOutputBytes` (1 MiB by default, 16 MiB at most); the rest is read and
  discarded, so a noisy program is never blocked, and `stdoutTruncated`/`stderrTruncated` say so. At most 8 runs are active.
  Standard output and error are decoded as UTF-8.
- **Descendants.** On a timeout or cancellation the program **and everything it started in its process group** (POSIX) or job
  object (Windows) is killed; the same happens when the program ends by itself with a descendant still running, so nothing a
  call started outlives it. A descendant that left the group (`setsid`, a daemon) or escaped the job is not tracked on POSIX; its
  inherited pipes are given one second after the program ended, then abandoned.
- **Cleanup.** A temporary space is removed by `withTemp`, by `removeTemp`, when the test process ends normally (also after a
  failed or cancelled spec), and the runner deletes `temp/` before and after each run, so an earlier run killed by the deadline
  leaves nothing behind for the next. Spaces have unique random names, so concurrent runs and sessions never share one. A
  `SIGKILL` of the test process, a power loss or a Windows file still locked by a program leave files in the build tree until
  the next run of the same named test (the runner's deletion) or `ctest` cleanup of the build directory.
- **Failure.** A spec assertion that is false, a refused or unauthorized call that is not caught, and a program that timed out
  or could not start when the spec expects success all fail the test, and so CTest and CI (exit status of the runner).

This is an authorization, **not a sandbox**: an authorized program runs with the user's authority and sees the filesystem and
the network; the specs and the page are trusted code, as everywhere in this guide. The temporary-space check resolves symbolic
links, but a program can still be told any path it can reach. Treat the grant like handing the test your shell for that one
binary, and use an isolated CI environment without secrets for untrusted projects. The wf scripts of the developer environment
(`wf run`, `wf exec`) still run in a browser Worker with no process capability, and the manifest's permissions cannot add one
(see [the developer environment](DEVELOPER_ENVIRONMENT.md#scripts-wf-run-wf-exec)); a tool that must execute a program is a
spec of this path.

### Recipe: XML (ATDF) to JSON, then the native tool

[`test/web/jasmine-consumer/host.spec.js`](../test/web/jasmine-consumer/host.spec.js) holds a complete example run by CTest
(`ConsumerJasmine-host`): it fetches the fixture `atdf-mini.xml` from the document root, converts it with `DOMParser`,
writes the JSON to a private space and runs a native program on it, asserting its output and a failing exit code with its
stderr. `DOMParser` exists in the page the specs run in, unlike the wf Worker; a converter that must run in the Worker needs a
pure-JavaScript XML parser installed with `wf install` from the registry and locked, and cannot start the native tool. To
build the converter as a distributed tool, keep it a TypeScript module compiled by `wf build` and import it from the spec; a
native window is still required to run it.

To run it in a consumer project: add `PROGRAMS "$<TARGET_FILE:your-tool>"` to the helper call, compile the TS specs to classic
JavaScript beforehand (this path does not compile TypeScript), keep the fixtures next to the spec and read them with `fetch`
from the application's document root. Provisioning is the one configure-time download of the pinned `jasmine-core` (or
`WEBFRONT_JASMINE_ARCHIVE_DIR` offline); the run needs no Node.js, Python or network. Measured on Linux with CEF (Release, this
repository): the twelve specs of `ConsumerJasmine-host` took 5.6 s in total, of which 3.0 s is the deliberate wait proving a
killed descendant stays dead and about 1.3 s the start of the window, which a passing one-spec test also pays.

Platforms: the contract is tested on **Linux** (CEF, `xvfb-run`). `Process.hpp` also has a POSIX path for macOS
(`posix_spawn`, process group; the descriptor-closing action is glibc-only, so other descriptors rely on `FD_CLOEXEC`) and a
Windows one (`CreateProcessW`, job object, restricted handle inheritance). The same CTest cases cover them, but they have not
been run on those systems yet: **treat macOS and Windows as unsupported until CI has validated them.**

## Screenshots and image sequences

A test build can capture the **web content** of the window (without native decorations) as PNG images, for example to
build a README animation in CI. Each named test owns `webfront-jasmine/<name>/captures/`, emptied before every run. The
specs reach it through the global `webFrontCapture`:

```js
it('shows the board running', async () => {
    await webFrontCapture.screenshot('start');                       // captures/start.png
    const frames = await webFrontCapture.record('run', {seconds: 4, framesPerSecond: 12});
    // captures/run/0001.png ... 0048.png; frames is the number written
});
// Or bracket a scenario yourself: startRecording(name, framesPerSecond) ... stopRecording() -> frames
```

Encode the sequence outside WebFront: `ffmpeg -framerate 12 -i captures/run/%04d.png run.webm` (or gifski for a GIF).
Names have 1 to 64 letters, digits, `_` or `-`, so a page can neither leave the directory nor pick a path; rates go from
1 to 30 images per second and a recording stops at 1800 images. Only one recording runs at a time, on its own thread,
so the bridge and the page keep running while it records. A screenshot or the end of a recording does not block the
server thread either: the bridge callbacks only start the work on a worker thread and the page polls for its end (every
20 ms) before the Promise settles. Capturing is paced by the engine: a slow capture drops
instants rather than bursting, so the number of images can be below `seconds * framesPerSecond`. Stopping a recording
whose window disappeared throws the capture error. A recording that ends by itself (an error, or the 1800 images limit)
keeps its slot, and its result, until `stopRecording()` collects it; if the page never does (it reloaded), the next
`startRecording` discards it, and a new page replacing the instrumented one stops a recording still running. A
screenshot keeps its result for the caller that started it: a second `screenshot` while one is running or uncollected is
refused, so concurrent calls never receive each other's image (run them one after the other); a new page discards an
uncollected one. A recording name is used once per run: a second `startRecording` with the same name
is refused rather than erasing the first one's images. The `webFrontCapture` methods do not depend on `this`.

From C++, `webfront::frontend::capturePng(timeout)` returns the PNG bytes (or a diagnostic) of the open window from any
thread other than the one running its event loop, and `webfront::testing::CaptureStore` writes them to a directory
(`screenshot`/`stopRecording` wait; `beginScreenshot`/`pollScreenshot` and `requestStop`/`pollRecording` do not).
One window per process is supported.

| Frontend | Capture |
| --- | --- |
| WebKitGTK | Supported (`webkit_web_view_get_snapshot`, GTK 4.6 or newer, otherwise the call fails with a diagnostic), validated under `xvfb-run`; the sandbox opt-out variable is not needed by the capture itself. |
| WKWebView | Supported (`takeSnapshotWithConfiguration:`, encoded as PNG by AppKit); the request runs on the main thread, which must be running the window's event loop. A hidden window (`visible = false`) is not qualified: its snapshot may be empty. |
| WebView2 | Supported (`CapturePreview`, PNG); the request runs on the window's STA thread, which must be pumping its message loop. |
| CEF | Supported through the DevTools protocol (`Page.captureScreenshot`), posted to the browser's UI thread, which must be running its message loop. It opens no DevTools window; it was validated with the default policy, where DevTools are enabled. |

This is an opt-in test facility with the same trust model as the rest of this guide: it is compiled only with
`WEBFRONT_JASMINE_TESTING`, and the captured page can contain secrets it displays. Automation/MCP does not expose
captures yet.

A capture that is waiting for the window when it closes, whether the engine already started it or the window's thread
had not yet got to it (its event loop may have stopped), is answered with "the window is closed" instead of waiting for
its timeout; a capture requested after the closing is refused at once. A page that replaces the instrumented one stops its recording and finds the slot free when the new page
attaches (the old worker is waited for at most half a second; one blocked inside a window that no longer answers keeps
the slot until it returns).
