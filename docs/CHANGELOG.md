# Changelog

All notable changes to WebFront, newest first. The format follows [Keep a Changelog](https://keepachangelog.com/) and
[Common Changelog](https://common-changelog.org/); see [Changelog format](../.github/CONTRIBUTING.md#changelog-format).

Each version opens with a short summary, then lists `Breaking`, `Added`, `Changed`, `Fixed` and `Security` entries
(empty categories are omitted). An entry is one line that says what changes for you and ends with its references.
Mechanisms, error codes and platform details live in the cited ADR, guide or pull request. The earlier, detailed text of
these entries stays in the GitHub releases and the Git history.

## Unreleased

### Breaking
- A custom frontend whose `open()` takes `const ViewLaunch&` declares `static bool providesViewProof()`, `true` when it gives the proof to its view and `false` otherwise, or the developer environment no longer compiles with it: undeclared, its proven view would leave project writes and C++ commands unguarded (#680, ADR-027, `docs/FRONTENDS.md`).
- `cppFunction` (and `Space::cppFunction`) refuses the names starting with `webfront.inspector.`, reserved to the inspectors, with `std::invalid_argument`: rename such a function, and its `webFront.cppFunction(...)` calls in the page, to a name of your own (#680, ADR-027).

### Added
- Open the inspectors from the WebFront tab's menu of the developer environment: each panel shows in a floating window of the page and can be detached to a native secondary window (WebKitGTK, WKWebView, WebView2) or, with `trustLoopbackClients`, a tab of the system browser; the choice and the place are kept per project (#680, ADR-027, `docs/DEVELOPER_ENVIRONMENT.md`).
- See in the **Functions** panel of the inspectors what the application registered: each `cppFunction` with its signature written in TypeScript, the file and line of its registration, whether it receives the calling `UI`, its calls, failures, last error and durations, then the JavaScript functions that C++ calls; sort, search, and choose the refresh period (#681, ADR-027, `docs/DEVELOPER_ENVIRONMENT.md`).
- See in the **Browsers and tasks** panel the connected browsers (embedded view or external browser, local client, pending writes and calls), the running tasks of the server and the application's terminal commands (#681, ADR-027).
- Read the same registry from C++ with `app.registeredFunctions()` (#681, ADR-027).
- Declare what the inspectors show with `app.watch(name, source)`, whose handle removes the watch when it is destroyed; the Watches panel follows their registrations and removals. Outside the developer environment and in a distribution build, `watch` registers nothing (#680, ADR-027).
- Embed a plain interface directory (HTML, JavaScript, CSS, vendored files) from CMake with `webfront_embed_directory(app ui CLASS AppUI)`, without the developer environment: the build regenerates the provider when a file changes, and `fs::DocumentRootFirstFSWith<AppUI>` lets a development root serve in front of it (#675, `docs/EMBEDDED_ASSETS.md`).
- Set from the configuration `Referrer-Policy` (`contentPolicy.referrerPolicy`: `"no-referrer"` forbids explicitly that a capability in your URLs leaves in `Referer`), `X-Content-Type-Options: nosniff` (`noSniff`) and the `Cache-Control` of HTML pages (`htmlCacheControl`), on files, route and generated replies unless a reply sets its own (#674, `docs/CONTENT_POLICY.md`).
- Keep a capability carried by your URLs out of WebFront's logs with `app.addSecretPrefix("/s/")`: the segment after the prefix is logged as `*` (`/s/*/index.html`) for files, routes, WebSocket upgrades and their failures, whose error messages are not logged (#673, `docs/HTTP_ASSETS.md`).
- Read the path from which a browser opened its bridge, by default its page's directory (within its space under `app.spaces`), with `ui.path()`; a WebSocket upgrade is accepted on any path a file would be served from, and refused as a file request is otherwise (#672, `docs/SPACES.md`).
- Serve several independent users from one process with application spaces: `app.spaces("/s/", resolver)` gives each `/s/<id>/` its own `cppFunction`s, page handlers, broadcasts and relative routes (`WebFront::Space`), serves the files under the prefix, lets the resolver refuse a space with its own reply, and tells the application when a space's last page closes (#671, `docs/SPACES.md`).
- Compile test files faster: include `<webfront/testing/bdd/Core.hpp>`, free of `<chrono>`, `<thread>` and `<fstream>`, and only the parts a file uses (`bdd/Matchers.hpp`, `bdd/Approx.hpp`, `bdd/Eventually.hpp`, `bdd/Runner.hpp` for `main()`); `Bdd.hpp` still includes them all (#669, `docs/BDD.md`).
- See both operands of a failed comparison with `checkEqual(a, b)`/`requireEqual(a, b)`, and write your own types in reports by specializing `webfront::bdd::Printer` (#669, `docs/BDD.md`).
- Compare floating-point values within a tolerance with `approx(v)`, `.epsilon()` and `.margin()`, Catch2's `Approx` semantics, with the expected value and tolerances in the failure (#669, `docs/BDD.md`).
- Run code after the last scenario with the third, optional `runEnding` function of `webfront::bdd::Listener`, called before the summary and the JUnit report whatever the outcome, even when a run- or scenario-starting listener threw; an exception of a run listener fails the run and appears in the JUnit report (#669, `docs/BDD.md`).
- The test helpers, `webfront_add_jasmine_test` included, export the real `XDG_DATA_HOME` and `XDG_CACHE_HOME` as `WEBFRONT_TEST_REAL_XDG_DATA_HOME` and `WEBFRONT_TEST_REAL_XDG_CACHE_HOME`, so an application under test can keep the throwaway WebKitGTK profile away from the programs it starts ([#664](https://github.com/ambroise-leclerc/WebFront/issues/664), `docs/CONSUMER_TESTING.md`).

### Changed
- The inspectors show where each `cppFunction` was registered; existing calls of `cppFunction` and `Space::cppFunction` compile unchanged (#681, ADR-027).
- `WebFront.js` opens its WebSocket on the directory of the page (`ws://host/dir/` instead of `ws://host`), which tells the server a page's space (#671) and lets an application be deployed under a sub-path of a site (#672).
- A request whose path holds a control character (`%00`...) is refused with 400, as a `..` already was, instead of being looked up as a file (#672).
- A `+` in the path of a file request is a literal `+`, as for the routes, the spaces and `ui.path()`: `/a+b.html` serves `a+b.html`, no longer `a b.html`, which `/a%20b.html` names (#672).

### Fixed
- A closed WKWebView secondary window, or one that another window covered, no longer leaves its page suspended and connected to the application: on macOS 14+ a secondary window is never suspended while inactive (#681, `docs/WKWEBVIEW.md`).
- An invalid content policy (an origin of `bridgeAllowedOrigins` or `navigationAllowedOrigins`, a `referrerPolicy`, a `htmlCacheControl`) throws `std::invalid_argument` before the server binds its port, as documented, instead of an "address already in use" error when that port is taken (#674).
- `DeveloperBrowserIntegration-workspace` gives the relaunched bootstrap page the same 60 s to start as the first one, instead of 10 s, which a loaded runner exceeded while the page verified its cached environment (#660).
- Applications started from the terminal of a Snap-packaged editor (VSCodium) now run on WebKitGTK without cleaning the environment by hand; `WEBFRONT_KEEP_SNAP_ENVIRONMENT=1` opts out (#662, `docs/WEBKITGTK.md`).

### Security
- Require an admitted client for developer project writes and native terminal commands when the frontend proves its view (ADR-027, #680).

## [1.2.0] - 2026-11-24

WebFront 1.2 ships with developer environment 1.2.0. The application itself becomes the MCP server that a coding agent drives
(`enableAutomation`), and it can open a second native window, serve HTTPS and run beside other applications on a free port.
It has breaking changes: `WebView2Options` becomes `ViewOptions`, `webfront-agent` is removed and the CEF profile moves; each
entry below gives its migration.

### Breaking
- `WebView2Options` is now `ViewOptions`, shared with WKWebView: `userDataFolder` moves after `launch`, so a positional initialization of `WebView2Options` stops compiling, and so does a designated one that writes `.userDataFolder` before `.contentPolicy`, `.window`, `.visible` or `.launch` (write it last) (#652, `docs/FRONTENDS.md`).
- `webfront/automation/Enable.hpp` no longer compiles with `WEBFRONT_DISTRIBUTION`: gate `enableAutomation` out of the distribution build, as the guide already advised (#632, ADR-025).
- `webfront-agent` is removed with its JSON CLI, MCP on stdio, `--serve`, `--attach`, `--list` and `--print-mcp-config`, as are `WEBFRONT_BUILD_AUTOMATION`, `WEBFRONT_AUTOMATION_FRONTEND` and `webfront_add_agent()`: connect the MCP client to the URL the application prints (#632, ADR-025, `docs/AUTOMATION.md`).
- `AutomationOptions::listen` becomes `directory` and may stay empty (a private directory named after the session is created), `Automation::endpoint()` becomes `directory()`, and the JSON-lines attachment channel (`ChannelServer`, `ChannelClient`) is gone (#632).
- The CEF profile is now `cef_cache/<executable>`, not `cef_cache`: an application starts with an empty profile (localStorage, cookies); to keep the data, move the old contents of `cef_cache` into `cef_cache/<executable>` once (#617, `docs/HOST_INTEGRATION.md`).

### Added
- Open a second native window with `app.openSecondaryWindow(page, WindowOptions)` (WebKitGTK, WKWebView, WebView2; elsewhere `std::logic_error`, which `app.canShowSecondaryWindow()` predicts): it shares the server, the `cppFunction`s, the `jsFunction` subscribers and the view proof of the first window; `app.requestClose(page)` closes it, and closing the first closes the others (#616, `docs/FRONTENDS.md`).
- Write your own scenarios with the public `webfront::bdd` framework: `<webfront/testing/Bdd.hpp>`, the `WebFront::Bdd` target, the installable `WebFrontBdd` package and `webfront_bdd_discover_tests()` (prefix, properties, shards) replace the internal `test/unit/Bdd.hpp` and `bdd_discover_tests`, and follow WebFront's versions (#647, `docs/BDD.md`).
- Configure a stable MCP port and application-supplied Bearer token, or explicitly disable authentication for trusted development (#637, ADR-026, `docs/AUTOMATION.md`).
- Connect an MCP client to the application itself: `enableAutomation(app, {.grants = {...}})` serves MCP over Streamable HTTP on a loopback port of its own, prints its URL and writes a private `mcp.json` with the URL and a session Bearer token, so several applications run side by side with no other program (#632, ADR-025, `docs/AUTOMATION.md`).
- Use a custom network provider with `DeveloperAppWithNetwork`. The `BasicDeveloperApp` alias selects the existing TCP provider (#631).
- Pass the MSVC code analysis on the automation host probe and the discovery directory of `Enable.hpp` (C6031, C6244), which stopped the Windows desktop job of a dispatch or pull request at its build (#638).
- Serve HTTPS and WSS with the optional header-only TLS 1.3 listener and reload its PEM identity after renewal (#631, `docs/TLS.md`).
- Start an application with no port setting when `9002` is taken: an empty `WebFrontConfig::port` falls back to a free port, a chosen port is never replaced, and CEF and WebKitGTK give each live instance its own profile directory (#617, `docs/HOST_INTEGRATION.md`).
- Start several applications side by side: the default port follows `WEBFRONT_PORT` (a number from 0 to 65535, `0` asks for a free port, `9002` remains the default), the examples take `--port`, the server announces the port really bound, and a failed bind names the address, the port and the cause (#617, `docs/HOST_INTEGRATION.md`).

### Fixed
- Check the leftover STA of a WebView2 window only after a page that ran to its end, not after one that closes the window on a load failure: the runtime can keep a COM initialization of its own then, in about one `show()` in 500 under load, and the limit is documented with its measurements (#638, `docs/PROJECT_STATUS.md`).
- Refuse every `Sec-Fetch-*` request header on the MCP listener, including `Sec-Fetch-Dest`, in both authenticated and unauthenticated modes (#637).

## [1.1.1] - 2026-11-20

WebFront 1.1.1 ships with developer environment 1.0.1. It repairs the desktop CI jobs that the 1.1.0 release exposed (a Windows
code-analysis error in the PTY, a macOS socket option error) and removes the GCC 13 warnings. No breaking change.

### Added
- Run the MSVC code analysis in a pull request into `develop` that changes Windows code, and list the reruns each job needed before going green with `FlakeReport.py --days` (#628, `docs/TEST_TIERS.md`, `docs/TEST_PERFORMANCE.md`).

### Fixed
- Pass the compiler analysis on the Windows pseudoterminal when it cannot prove the attribute list non-null (C6387), stop a unit test from opening the developer's real browser (the directory of its replacement opener was removed before the shell found it), and keep an accepted connection whose `TCP_NODELAY` the system refuses, as macOS does after a reset (#628).

## [1.1.0] - 2026-11-20

WebFront 1.1 ships with developer environment 1.0.1. It adds POST routes with a bounded body, application-selected programs
in a native pseudoterminal, and a faster, less flashy start of the native windows. No breaking change.

### Added
- Answer `POST` on a dynamic route that opts in with `http::RouteOptions::postBodyLimit`: the body, bounded by `Content-Length`, reaches the handler as `RouteRequest::body`; `Response::accepted` (202) and `Response::unauthorized` (401) have their reason phrases (#619, `docs/HTTP_ASSETS.md`).
- CEF supports `WindowOptions::background` and shows its window when the first document has loaded or after one second (#611, `docs/FRONTENDS.md`).
- Print a hint on standard error when the development file system serves a stylesheet whose `font-family` lists four or more costly families (#611, `docs/WEBKITGTK.md`).
- Run the start-up time of the native windows, cold and warm (cold is fully cold on Linux only), as `<Frontend>StartupTime-cold` and `-warm` (label `startup`) with generous limits, timed from the start of the program so that the frontend's initialization counts (#611, `docs/WEBKITGTK.md`).
- Run application-selected programs in a native pseudoterminal with an optional xterm.js page adapter and replay across reloads (#613, `docs/PTY.md`).
- Skip GStreamer's initialization, and its plugin scan, in WebKitGTK for an application whose pages never use audio or video, with `WindowOptions::media = false`; the default keeps media (#611, `docs/WEBKITGTK.md`).
- WKWebView supports `WindowOptions::background`, translucent colours included, and `WEBFRONT_STARTUP_TRACE=1` (#611, `docs/WKWEBVIEW.md`).
- WebView2 supports `WindowOptions::background` and `WEBFRONT_STARTUP_TRACE=1` (#611, `docs/WEBVIEW2.md`).
- Choose the colour of the WebKitGTK view under the page with `WindowOptions::background` (WebKitGTK; the other embedded frontends are listed here), and read the start-up milestones of a WebKitGTK view with `WEBFRONT_STARTUP_TRACE=1` (#611, `docs/WEBKITGTK.md`).

### Changed
- The whole C++ code base is reformatted with `clang-format` and every source starts with a normalized header (SPDX, author, creation date, role, thread and allocation model, reading hint); CI checks both (`SourceHeaders` case, `.github/workflows/SourceFormat.yml`), and `.git-blame-ignore-revs` skips the reformatting in `git blame` (#653, `.github/CONTRIBUTING.md`).
- WebKitGTK takes a `startupTimeout` (30 s by default), and WebView2 refuses a secondary window once the first one closed, even for a page already open (#652).
- Open every native window from one `ViewOptions` (`WKWebViewOptions` and `WebView2Options` are aliases), whose test injections reach the secondary windows too (#652, `docs/FRONTENDS.md`).
- Write a native frontend from shared headers: `EmbeddedFrontend` (facade and every `open()` overload), `WindowRegistry`/`InterfaceSession`, `StartupTrace` and `Presentation`, tested on Linux with fake windows (#652, `docs/FRONTENDS.md`).
- Reveal the WKWebView window when the document has loaded or after one second (#611, `docs/WKWEBVIEW.md`).
- Reveal the WebView2 window after the first frame or one second (#611, `docs/WEBVIEW2.md`).
- Show the WebKitGTK window when the page reports its first frame, or when a one-second timer fires, even if the page has not painted, instead of at once; the embedded pages, the starter and the examples use the cheap `sans-serif` and `monospace` font stacks (#611, `docs/WEBKITGTK.md`).

### Fixed
- Accept HTTP request headers with optional spaces or tabs after the colon, tabs in values, and empty values (#621).
- Read an obsolete line fold in a request header as one space (RFC 9112, section 5.2), not as the two lines glued together: a `Content-Length` folded as `1` and `0` was read as `10` (#619).
- Keep the input focus of the developer terminal after a click on the page around it (title, notice, blank areas) while the terminal has the window, selection and controls excepted (#608).
- Remove the white flash of an embedded view between the developer environment's bootstrap page and the project's page, by giving the window the environment's dark colour (`#141821`) when the application sets none; on macOS the view then no longer paints its own white behind it (#611, `docs/WKWEBVIEW.md`).
- Select detected GPU architectures supported by nvcc by default for the host-loop CUDA renderer (`docs/HOST_LOOP_EXAMPLE.md`).

## [1.0.0] - 2026-11-17

WebFront 1.0 ships with developer environment 1.0.0. This release adds host-loop integration, developer tools and
platform qualification; see the breaking changes below when upgrading.

### Breaking
- WebKitGTK defaults to development mode with its sandbox disabled; packaged applications must select `WEBFRONT_DISTRIBUTION=ON` or `webfront_use_distribution(target)` to require it, independently of Debug/Release (#596, `docs/WEBKITGTK.md`).
- Reserve more command names for the terminal: an application command named `ls`, `cat`, `tree`, `mounts`, `cd`, `pwd`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `edit`, `find`, `grep`, `head`, `tail`, `less`, `stat`, `du` or `df` is now refused by `webfront::Commands::add` and must be renamed (#575, ADR-023).

### Added
- Capture numbered PNG sequences with a fixed simulation clock using `WebFrontHostLoop --capture-frames`; `--capture-demo` hides statistics and switches materials continuously. Illustrate the bilingual README with the CUDA checkered ball followed by illuminated glass (#602, `docs/HOST_LOOP_EXAMPLE.md`).

- Qualify real X11 Canvas/WebGL presentation on NVIDIA EGL, the GNOME file portal and fullscreen Life React on Orin Nano Super; retain captures, an EGL diagnostic and the limits of nested Wayland testing (#528, `docs/JETSON.md`).
- Add an isolated, low-priority Jetson Orin hardware workflow with persistent caches, complete CEF-off tests, WebKitGTK integration, required CUDA regression and workflow security checks (#529, `docs/JETSON_RUNNER.md`).
- Turn the `WebFrontHostLoop` example into a ray-traced Boing Ball computed by the host's own loop on the CPU or with an optional CUDA unit, shown full-window by an embedded page that receives at most one image at a time; a glass ball with coloured beams is the workload that separates the two engines. Measured on a Jetson Orin Nano (#530, `docs/HOST_LOOP_EXAMPLE.md`).
- Generate a per-executable AppArmor profile for application packaging with `webfront_generate_webkitgtk_profile`; select the distribution policy per target with `webfront_use_distribution`, without adding a runtime (#596, `docs/WEBKITGTK.md`).
- Look at the project from the terminal with `ls`, `cat`, `tree` and `mounts` (read-only), and at the installed packages with `wf list --tree`, `wf why <package>` and `wf info <package>`; a `wf` command typed without its prefix answers with the line to type. The `wf` prefix stays (#575, ADR-023, `docs/DEVELOPER_ENVIRONMENT.md`).
- Add WebFront to an application that keeps its own loop: `app.start()` opens the system browser and returns at once, `app.requestClose()` ends an embedded window's `show()` from any thread, and `webfront::UIThread<App>` runs an application on a secondary thread where the frontend allows it (WebView2). New `WebFrontHostLoop` example (#567, `docs/HOST_INTEGRATION.md`, ADR-007).
- Tell an agent at first contact how an application is developed (C++ with a TypeScript/TSX interface in `ui/`, `wf dev`) and that it can run `wf` itself in JSON or MCP: a README section before the tutorial, `webfront_write_agent_guide()`, and notices at configuration, in the terminal and at startup (#561, `docs/DEVELOPING_WITH_AGENTS.md`).
- Add the `app` starter (`wf create app`: a minimal TypeScript interface without dependency), `wf help <command>` and `wf help agents`, and make a missing manifest, a missing entry and an unknown starter answer with the next steps. Environment 0.9.2 (#561, `docs/DEVELOPER_ENVIRONMENT.md`).
- Build and test with GCC 13 on a hosted Linux aarch64 runner in the weekly and `master` checks (`gcc13-arm64`), and let `setup-cmake` install CMake on aarch64 (#548, `docs/TEST_TIERS.md`).

### Changed

- Launch the developer environment from its verified cache without rewriting it while its published files match the archive: a `wf` command of the tests starts in 1.6 s instead of 2.5 s (#570, `docs/DEVELOPER_ENVIRONMENT.md`).
- Replace Catch2 with `test/unit/Bdd.hpp`, a single-header BDD framework: scenarios are `Scenario` objects whose `given`/`when`/`then` sections are lambdas, checked with `require`/`check`. The tests no longer download or compile Catch2; `tests` takes `--exact`, `--list`, `--junit` and the former filters, order and shard options (ADR-012, `docs/TEST_TIERS.md`).
- Wait for asynchronous states in unit tests with `requireEventually`, `checkEventually`, `requireReady` and `checkReady` of `Bdd.hpp`, which report the delay and the time waited and replace nine local helpers; `WEBFRONT_TEST_TIMEOUT_FACTOR` lengthens the delays of a slow job (#582, `docs/TEST_TIERS.md`, ADR-012).
- Run each developer integration part and each wf command of the tests in a process of its own again: the shared windows of #544 and #556 made the driver cases about a third slower on Windows and failed more often. Remove the timing analysis tools and their CI steps, and repeat the desktop suite through one path, with `desktop_passes` on a manual dispatch (#570, `docs/TEST_PERFORMANCE.md`).
- Close the test driver's window with `requestClose()` instead of asking the page: a spec that blocks the page now ends with the driver's timeout report instead of a forced exit (#570).
- Keep the configure downloads of the tests (bootstrap environment, test registry, jasmine-core) once per machine, in `WEBFRONT_DOWNLOAD_CACHE`, and try a failed download three times: a new build tree links the verified archives instead of downloading them again (#570, `docs/DEVELOPER_ENVIRONMENT.md`).
- Declare GCC 13 as the best-effort S2 minimum for the library and examples, document the GCC 14 exit clause and qualified Clang scope, and add GCC 13 ASan/UBSan fast tests (#525, ADR-022).

- Open the developer page on the interface, over the whole window, once the project has a preview; the terminal moves to a floating, movable window behind a tab on the right edge that shows the session's state, and keeps its session when collapsed. Its shortcut is F9, or the manifest's new optional `developer.terminalShortcut`. Introduced in environment 0.9.1 (#539, `docs/DEVELOPER_ENVIRONMENT.md`).
- Make the default test build the quick one: the heavy unit tests compile four to a file, the MSVC code analysis and the reproducibility proofs are off unless `-DWEBFRONT_MSVC_ANALYZE=ON` or `-DWEBFRONT_TEST_REPRODUCIBILITY=ON` asks for them. The CI passes every option explicitly and checks what it checked before (#546).

### Fixed
- Build the host-loop capture example with AppleClang when ignoring the frame number (#602, `docs/HOST_LOOP_EXAMPLE.md`).
- Deduplicate the host-loop example's frame requests through rendering until each delivery starts, and expose its status messages to assistive technologies (#599, `docs/HOST_LOOP_EXAMPLE.md`).
- Make the host-loop example reject `--capture` without a native window or with `--benchmark`, and return a failure when the capture cannot be saved or the window closes before it completes (#599, `docs/HOST_LOOP_EXAMPLE.md`).
- Enforce the WebKitGTK distribution policy across translation units and inherited environments; bound launcher checks and restore the development opt-out before callbacks or system-browser fallback (#596, `docs/WEBKITGTK.md`).
- On Windows, a slow HTTP client no longer blocks the publication of developer resources, and removing a log sink no longer returns while one of its callbacks is still running (#542, `docs/validation/ci-flakes-542.md`).
- The Linux interface loop shuts down without leaving work behind, and the automation deadlines are decided consistently when a window is ready at the moment its startup expires. The ThreadSanitizer job no longer needs its GLib suppression (#542, `docs/validation/ci-flakes-542.md`).
- `WebView2Lifecycle-native-early-close` no longer fails under load: the COM initialization that the WebView2 runtime keeps after a window closed during its startup is a documented limit, and the frontend's own apartment has a unit test (#564, #570, `docs/PROJECT_STATUS.md`).

- Restore the terminal prompt with exit code 1 when `wf dev` starts without a project or a selectable entry; compilation errors still keep watching for edits. Environment 0.9.2 (#560).
- Read all six developer integration reports in CTest after a part fails, and keep the successful parts' dependent tests runnable, also when the session's native deadline ends it (#556).
- On Windows, `wf install`, `wf build` and `wf test` no longer fail with `rename: Access is denied` while another process, such as an antivirus scan, still reads the files just written: the developer store tries the rename again for up to 635 ms; `wf test` also empties the directory of an interrupted run (#542).

- Build with `-DWEBFRONT_EMBED_CEF=ON` on Linux aarch64 instead of failing on `-m64`, and cross-compile CEF: WebFront pins CEF 156.0.28, whose package selects the archive and `PROJECT_ARCH` from the target (#523).


## [0.9.0] - 2026-11-10

Last minor revision before 1.0, with the developer environment at the same number. It lays the groundwork for the
system services of [ADR-020](adr/README.md): confined writable storage, native window access, an interface-thread executor,
ranged and dynamic HTTP routes. **One migration:** libraries and developer environments older than 0.6.5 must be rebuilt,
because they pin a distribution address that no longer bootstraps. Developer environment 0.9.0 is the first one published
by the `release-developer` workflow.

### Breaking

- Rebuild libraries and developer environments older than 0.6.5 against current sources: they pin the former distribution address, which redirects to `https://dist.webfront.fr/` without the CORS headers a browser needs, so `bootstrap("developer")` no longer starts (#478, ADR-021).

### Added

- Send long strings, homogeneous lists of strings, tuples, typed arrays and bitsets through the bridge, raise typed errors with application codes, and let a `cppFunction` callback return a `webfront::ResultTask<R>` or another sender (#456–#459, ADR-003, `docs/ASYNC_EXECUTION.md`).
- Add `fs::Storage`, `fs::NativeStorage` and `fs::MemoryStorage`: writable storage confined to declared roots, with atomic commits (#460, ADR-002).
- Add `frontend::NativeWindow` and `UI::nativeWindow()`: typed access to the native window of an embedded view (#463, ADR-020).
- Add `frontend::UIExecutor` and `BasicWF::uiExecutor()`: a sender that completes on the interface thread, for every frontend (#462, ADR-007).
- Add `BrowserWithUILoopFrontend`: the system browser with an optional minimal interface loop (#462).
- Add `app.addRoute(prefix, handler)`: dynamic `GET`/`HEAD` routes with streamed bodies (#454).
- Add `Range` support (`206`, `416`) for files of known size (#455).
- Add `app.addReadRoute`, `ui.grantRead` and `webFront.bound`: HTTP reads bound to one browser client (#469, ADR-020).
- Add `UI::isEmbeddedView()` and `UI::isLocalClient()`: a connection proves it comes from the embedded view (#453, ADR-020).
- Support `alert`, `confirm` and `prompt` in WKWebView, and `<input type="file">` and downloads in the embedded frontends (#451, #452).
- Add `webfront::automation::enableAutomation(app, options)`, `webfront-agent --list`, `--print-mcp-config` and the `call` operation, to drive an application's own window (#501, ADR-019).
- Add the `@webfront/services` package skeleton and `wf run services-package` (#461, ADR-010).
- Add the `release-developer` workflow that builds, tags and publishes the developer environment (#482, ADR-021).

### Changed

- Serve the developer distribution from `https://dist.webfront.fr/` (#478, ADR-021), as the breaking entry above explains.
- Stream unencoded files of known size instead of loading them whole (#454).
- Make `wf run pin` fail when the pinned URL leaves `baseUrl`'s origin, redirects or lacks CORS (#482).
- Make `wf check` without a `tsconfig.json` root the project's own `.d.ts`, `.d.mts` and `.d.cts` files (#501).
- Make `safeRelativePath` refuse the device names Windows reserves, and names ending with a dot or a space (#460).
- Diagnose asynchronous media and file-services specs before Jasmine's generic timeout (#504).

### Fixed

- Stop `Server::drain()` and `shutdown()` from throwing `std::logic_error` while a route handler is running (#469).
- Build the unit tests with GCC 16.1 and make the native storage tests independent of the umask (#517).

### Security

- Refuse a read grant to any connection that is not the embedded view, unless `WebFrontConfig::remoteFileTransport` declares the transport (#469, ADR-020).
- Refuse a root or directory that another POSIX user could move (`unsafeDirectory`), and a link or reparse point inside a storage root (#460, ADR-002).

## [0.8.0] - 2026-11-01

Automation, consumer tests and embedded engines. **No breaking change.**

### Added

- Add `PROGRAMS` and `webFrontHost.run(...)` to run authorized native programs from the specs of `webfront_add_jasmine_test` (#445).
- Add `makeTemp`, `withTemp`, `writeFile` and `readFile`: private temporary spaces for those specs (#445).
- Add `webFrontCapture.screenshot(name)` and `record(name, options)`: PNG captures of the web view (#468).
- Add `webfront::embeddedFrontendAvailable` and `webfront::EmbeddedWebFront`, usable without consumer-side macros (#467).
- Make `init`, `create`, `install`, `restore` and `embed` available to `webfront-agent`, each behind its own `--allow` grant (#466).

### Changed

- Fail at startup when an older environment is granted an operation it lacks, naming the operation (#466).

## [0.7.0] - 2026-10-31

Test and CI housekeeping, with one **breaking** change to CMake option names.

### Breaking

- Prefix WebFront's test, fuzzing, coverage and sanitizer CMake options with `WEBFRONT_`, for example `WEBFRONT_ENABLE_TESTING`. Unprefixed names and old cache entries no longer configure WebFront (#412).

### Changed

- Cut pull-request CI time: four-file unity batches, no example or MSVC analysis on pull requests into `develop` (#435, `docs/BUILD_TIME_STUDY.md`).
- Include `developer_integration` in the documented Linux CEF build commands (#434).

### Fixed

- Remove the Windows agent's native profile after a forced exit (#439).

## [0.6.0] - 2026-10-31

Release of the DOM, forms, React and automation layers, with **two breaking changes** to the C++ API: values of `UI`,
`JsFunction` and `JsFunctionSender` can no longer be built by hand, and `BasicWF` is no longer copyable or movable.
The largest version of the pre-1.0 series, condensed from 49 entries.

### Breaking

- Use the `UI` that `onUIStarted`, `subscribeUIStarted` or a callback hands over instead of constructing `UI`, `JsFunction` or `JsFunctionSender` yourself: their public constructors take the connection generation as an extra argument (#428).
- Do not copy or move a `BasicWF`: it is now non-copyable and non-movable, which the pointer held by `UI` requires (#400).

### Added

- Add `ui.document()`: per-browser DOM islands with batched writes and awaitable events (#380, #381, #384, `docs/DOM.md`).
- Add type descriptors and generated forms and inspectors, with an optional C++26 reflection layer (#383, `docs/FORMS.md`).
- Add the `@webfront/react` adapter: coalesced C++ stores and `Island` (#382, `docs/REACT.md`).
- Add `app.broadcast(name, options)(...)` and `app.forEachUI(function)` to reach every browser (#401).
- Add `webfront-agent`, local automation of the developer window through stdio MCP and a JSON CLI (#403, ADR-019, `docs/AUTOMATION.md`).
- Add consumer Jasmine/CTest integration with `webfront_add_jasmine_test` (#402, `docs/CONSUMER_TESTING.md`).
- Add `frontend::nativeWebViewAvailable` and `frontend::NativeOrBrowserFrontend` (#398).
- Take the native window's title and initial size from `WebFrontConfig::window`.
- License WebFront under Apache-2.0 OR CECILL-B, at the recipient's option.

### Changed

- Make `BasicUI` (`UI`) an assignable value type that holds a pointer to the application (#400).
- Enable native-window detection by default for `add_subdirectory` and CPM consumers (#397).

### Fixed

- Reject a stale `UI` after its connection identifier is reused: calls are dropped and typed calls fail (#428).
- Keep automation shutdown and cleanup reliable on Windows and under load (#416, #439).

## [0.5.0] - 2026-10-26

The developer environment becomes a complete, standalone `wf` toolchain, and the library moves under `include/webfront/`.
Three **breaking** changes. Condensed from 62 entries.

### Breaking

- Include headers as `<webfront/WebFront.hpp>`, `<webfront/http/...>` and so on. The former spellings are removed with no redirect headers (ADR-016).
- Replace `BasicWF::getLink(id)` with `findLink(id)`, which shares the link and is null once the browser is gone.
- Remove `webfront::Terminal`, `terminal::Shell`, `ToolManager` and the related filesystems; use the developer environment's terminal and `webfront::Commands` (#313).

### Added

- Add `wf run`, `exec`, `check`, `test`, `build`, `dev` and `restore`, and the `webfront.project.json` manifest and lock (#308–#310, `docs/DEVELOPER_ENVIRONMENT.md`).
- Add `webfront-wf`, the non-interactive driver, so tests, regeneration and CI need no JavaScript runtime or package manager (#318, #321).
- Make the developer environment build itself (#315).

### Changed

- Guard the minimal core's embedded assets with `MinimalCoreAssets` (#314, ADR-016).
- Run the browser integration suite through `wf test` (#311).
- Remove vendored `third_party/` sources and the retired PI starter (#319).

### Fixed

- Report a browser's disconnection and release its connection (`WebLink` emits `closed` once).
- Stop the bridge encoder from writing diagnostics on every message (#363).

## [0.4.0] - 2026-10-21

Repository and CI reorganisation. **No new library functionality, no breaking change.**

### Changed

- Move the public examples to `examples/`, keeping Hello World and Game of Life.
- Rework the README around a minimal Hello World and a step-by-step developer walkthrough.
- Combine per-change CI into three native desktop jobs; extended CEF, sanitizer and fuzz checks run weekly.

## [0.3.0] - 2026-10-21

Coroutines and the opt-in developer environment. One **breaking** change to the low-level API.

### Breaking

- Pass an external `networking::TaskScope&` to the low-level `WebSocket::create` and `WebLink` constructors, and drain it before releasing their dependencies. Server shutdown now joins a counting scope.

### Added

- Add `std::bitset<N>` arguments and results, packed as `Uint8Array`.
- Add `NativeApp::bootstrap("developer")`, the opt-in developer environment.
- Add `webfront::Task`, `app.spawn(task)` and `ui.spawn(task)` for application-owned coroutines.
- Add `ui.jsFunctionSender<R>(name)(...)`, a lazy sender cancellable through its receiver's stop token.

### Changed

- Fail decoding when a bridge integer is out of the requested C++ range, instead of invoking undefined behavior.

[Unreleased]: https://github.com/ambroise-leclerc/WebFront/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/ambroise-leclerc/WebFront/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/ambroise-leclerc/WebFront/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/ambroise-leclerc/WebFront/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.9.0...v1.0.0
[0.9.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/ambroise-leclerc/WebFront/compare/v0.2.0...v0.3.0
