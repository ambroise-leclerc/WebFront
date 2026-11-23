# WebFront project status

Release history belongs in the [changelog](CHANGELOG.md)
and Git; architecture decisions and their evidence are indexed in [the ADRs](adr/README.md).

## Compiler support

GCC 13 is the minimum for the library and examples, with best-effort S2 support: the
weekly and `master` PR ARM64 job builds everything without `-Werror`, runs the fast tier,
and repeats that tier under ASan/UBSan. Browser frontends are disabled in this job.
GCC 16 remains the reference compiler. Clang 23 is qualified in the extended jobs;
no older upstream Clang minimum is qualified. AppleClang and MSVC follow the native CI images.
Optional C++26 reflection and mddlog retain their separate compiler requirements.
The [support ADR](adr/ADR-022-profils-et-niveaux-de-support.md) defines the GCC 14 exit clause.

## Supported paths

WebFront is a header-only C++23 HTTP/WebSocket core plus a standalone developer environment.

- **C++ applications.** Link `WebFront::WebFront` from a CMake parent project. The default
  `webfront::WebFront` uses the system browser. Native applications explicitly select WebView2,
  WKWebView, WebKitGTK or CEF. Standalone and consumer builds detect the platform's native
  engine when its dependencies are available (outside test/fuzzing builds).
  `webfront_use_native_window(target)` provides target setup and a definition for selecting
  the native adapter with a browser fallback; see the [consumer example](../README.md#use-webfront-in-your-project)
  and [frontends](FRONTENDS.md). An application that keeps its own loop uses `app.start()` with the
  system browser, `app.requestClose()` from another thread, or `webfront::UIThread` where the frontend
  accepts a secondary thread (WebView2); a host loop cannot share the main thread with an embedded
  window. See [host integration](HOST_INTEGRATION.md).
  A second native window of the same application (`app.openSecondaryWindow()`, #616) exists on WebKitGTK, WKWebView and WebView2; CEF
  and the system browser refuse it with `std::logic_error` (`app.canShowSecondaryWindow()` tells it beforehand, also after `NativeOrBrowserFrontend` fell back to the browser), and the position of the window is left to the toolkit. The call waits
  for the interface thread: a `cppFunction` callback runs it on `app.uiExecutor()` rather than hold the server's thread.
  `ui.nativeWindow()` and the native dialogs of a secondary window's connection still refer to the first window: per-window native handles are not implemented.
- **Native pseudoterminals (opt-in).** `webfront::Pty` uses a Unix PTY on Linux and macOS or ConPTY on Windows 10 version 1809 and later. The page adapter requires a loopback bind and the application’s own browser origin. See the [PTY guide and standalone example](PTY.md).
- **Developer environment.** `NativeApp::bootstrap("developer")` loads the verified, pinned
  environment selected by `DeveloperReleaseData.hpp`. Its terminal provides `wf init`, `create`,
  `install`, `restore`, `run`, `exec`, `check`, `test`, `build`, `dev`, `preview`, `embed`, `list`, `why`, `info` and `help`, beside the terminal's own read-only `ls`, `cat`, `tree` and `mounts` (ADR-023). The manifest and lock describe
  dependencies, entries, providers, scripts and permissions. TypeScript/TSX compilation uses
  esbuild-wasm; type checking uses the project's locked TypeScript. Application commands use
  `webfront::Commands`. From environment 0.9.1, a project that has a preview opens
  on its interface over the whole window, with the terminal in a floating window behind a tab on the window's edge. See the
  [developer reference](DEVELOPER_ENVIRONMENT.md).
- **Distribution.** `wf embed` generates the application's compressed C++ provider.
  `NativeAppWithAssets<Provider>` runs without project files, developer callbacks or tool cache.
  Native-engine deployment remains application work. The [README walkthrough](../README.md)
  covers the console, developer and embedded Game of Life stages; Hello World and the host loop
  (`examples/host-loop/`, a Boing Ball ray-traced by the host on the CPU or with optional CUDA; see
  [the host-loop example](HOST_LOOP_EXAMPLE.md) for its measurements and limits) are also in the default build.
- **Developer automation (opt-in).** An application that calls `automation::enableAutomation` serves MCP itself over
  Streamable HTTP, on a loopback listener of its own with a session Bearer token, prints its URL and writes a private
  `mcp.json` client configuration; no other program is built or launched ([ADR-025](adr/ADR-025-mcp-http-dans-l-application.md),
  #632). Native grants select check/build/test/preview, bounded inspection, semantic actions, the setup operations and the
  owner's allow-listed callbacks (`call`). Several instances run side by side, each with its own URL and discovery record.
  A fixed MCP port and application-supplied token can keep a client configuration stable; authentication can also be
  explicitly disabled for development, with the local-process risks described in [Automation](AUTOMATION.md) (#637).
  Remote access is out of scope and automation does not compile with `WEBFRONT_DISTRIBUTION`. This does not isolate
  untrusted project code. [Automation](AUTOMATION.md) documents the listener, lifecycle, limits and the supported MCP profile.
  The `webfront-agent` launcher, its JSON CLI and MCP on stdio were removed. The environment 1.0.2 pinned in
  [`DeveloperReleaseData.hpp`](../include/webfront/system/DeveloperReleaseData.hpp) describes MCP;
  the starter guidance for stable client configuration is in the 1.0.3 sources and will reach `wf create` in the next
  environment release. Until then agents can follow `docs/DEVELOPING_WITH_AGENTS.md`. First-contact
  guidance for agents (README section, `docs/DEVELOPING_WITH_AGENTS.md`, `webfront_write_agent_guide()`, configuration and
  startup notices) comes with the library.
- **Maintaining WebFront.** The repository is itself a `wf` project, selected with
  `--manifest-dir js/developer`. Build and publish its environment using
  [PUBLISHING](../js/developer/tools/PUBLISHING.md). Regenerate the core's versioned headers
  with CMake's `webfront-embedded-assets` target; see [embedded assets](EMBEDDED_ASSETS.md).

No JavaScript runtime or package manager is required on the host. Dependencies are downloaded
and verified at configuration or first use. No third-party sources are vendored. The core's
closed resource list and 150 KiB decoded budget are guarded by `MinimalCoreAssets`
([ADR-016](adr/ADR-016-bootstrap-comme-socle.md)).

## Validation and diagnostics

Consumers can explicitly instrument their real application page with
`webfront::testing::JasmineTest` and register it through `webfront_add_jasmine_test`.
This optional [Jasmine/CTest path](CONSUMER_TESTING.md) supports precompiled classic JavaScript
specs and external process deadlines; it does not compile TS or isolate untrusted application code. It can also
write PNG screenshots and image sequences of the web content on WebKitGTK, WKWebView, WebView2 and CEF (#468), and let the
specs run programs the test's owner authorizes (`PROGRAMS`), with private temporary spaces (#445); its browser specs run on
Linux only (the runner's unit tests also run on Windows and macOS).

The CEF-off build and complete CTest commands are in [AGENTS.md](../AGENTS.md).
The suite includes deterministic C++ unit-test regressions, CMake consumer/configuration checks,
asset freshness checks and, with an enabled engine, real-browser Jasmine and lifecycle tests.
`Journey*` builds a UI with the rebuilt environment and local registry, then compiles and runs
its generated provider from an empty directory. Configured tests need no public network.

The [Jetson hardware runner](JETSON_RUNNER.md) adds trusted weekly and maintainer-dispatched
CEF-off, CUDA and WebKitGTK validation on the board (#529). It is S2 evidence and never
blocks a PR or merge when the workstation is unavailable. Three consecutive green weekly
runs and observed workstation usability remain acceptance criteria, not a claim made by
adding the workflow.

[Tiers](TEST_TIERS.md) defines the desktop and extended CI scope, including the GCC 13 Linux aarch64 job (#548) and its weekly CEF and WebKitGTK browser jobs (#524).
The [Jetson Orin qualification](JETSON.md) records the separate on-board work (#528):
the JetPack 7 upgrade identifies L4T R39.2.1, Ubuntu 24.04 and GCC 13.3 on an Orin
Nano Super 8 GB. The [final desktop record](validation/jetson-528/README.md)
qualifies X11 Canvas/WebGL presentation on native NVIDIA EGL contexts, the real
GNOME file chooser and parent export, and fullscreen Life React with its C++
panel updating after a click. No DMA-BUF opt-out was needed. A Wayland graphics
client passed under Weston 13 nested on X11; its missing portal-parent export
is retained as a negative result. Native GNOME Wayland is disabled in GDM and
was not qualified. NX/AGX physical behavior, a locked-down kiosk and fresh-flash
acceptance are not inferred from this existing-workstation qualification.
Ubuntu 24.04's AppArmor user-namespace policy
refuses WebKit's bubblewrap sandbox on that installation (`bwrap` cannot write the
UID map). Development builds explicitly disable the WebKit sandbox by default,
so native windows and Jasmine run without a system change. Distribution builds
select `WEBFRONT_DISTRIBUTION=ON` (or `webfront_use_distribution(target)`) and
require the sandbox; `webfront_generate_webkitgtk_profile` prepares the application's
per-executable authorization for normal package installation. Neither the library
nor its CMake helpers load policy on the development machine. See
[WebKitGTK](WEBKITGTK.md#development-and-distribution). JetPack 6 remains a
historical S3 path; the on-board X11 S2 profile names the measured L4T stack
rather than inferring a JetPack package set from an absent metapackage.
This does not erase the earlier JetPack 6 WebKitGTK qualification recorded in
[draft PR #558](https://github.com/ambroise-leclerc/WebFront/pull/558): 28/28
adapter/lifecycle cases and 7/7 Jasmine/fixture cases passed under Xvfb. That PR
is not merged; its file-reader fix is carried by #596. The JetPack 7 pass uses a
different OS, kernel, engine and security policy; the launch failure came from
that policy, not from a WebFront regression.
[Test execution and profiling](TEST_PERFORMANCE.md) documents fixture locks, complete-suite repetition, the intermittent
failures of the last runs and why shared browser sessions were retired (#570); each wf command is a driver process
on an OS-assigned loopback port. Manual transport measurements
remain separate from the short native transport contract tests.

The [#542 investigation](validation/ci-flakes-542.md) reproduces and fixes Windows developer publication held open by
an HTTP reader and concurrent fallback logger delivery. Its qualification passes 20 selected browser graphs on each
native engine and 1,000 repetitions of each Linux closure case. The historical Linux internal stall was not reproduced;
the report distinguishes that evidence from proven causes. The Windows early-close COM failure
([#564](https://github.com/ambroise-leclerc/WebFront/issues/564)) is reproduced and attributed to the runtime
([measurements](TEST_PERFORMANCE.md#the-webview2-early-close-failure-564)); its limit is listed below.
The [diagnostic workflow](TEST_PERFORMANCE.md#investigating-intermittent-failures) preserves failed attempts and HTTP
transcripts without automatic retries.

[Local-model qualification](validation/automation-models-2026-10-29.md) records five installed
models driving the real MCP window through an external LM Studio measurement adapter. It distinguishes
final source correctness (30/30 host-verified tasks) from completed verification observed by the model
(29/30 conversations). The report retains per-task data, token costs and the incomplete Flint case.
The maintained benchmark's built-in inference protocol remains Ollama; the measurement adapter is
not a supported launcher dependency or an additional benchmark CLI mode. These limited tasks do not
establish general model reliability. [ADR-019](adr/ADR-019-developpement-automatisable-et-agents.md)
accepts the implemented local contract and records these limits and its evidence.

The #376 regressions cover response-owned HTTP status storage, `Connection: close` and
failed gzip response leases occupying their admission slot until destruction. Repetition
recreates the entire fixture graph rather than repeating individual stateful cases.
The original WKWebView content-process termination was not reproduced locally, so its cause
remains unproven. Phase/spec progress, native watchdog failures and macOS crash/system logs
are retained for investigation; diagnostics alone do not establish a correction.

## Contracts and limitations

- Bridge calls from JavaScript always return Promises and reject on failure, including `void`
  callbacks. Untyped C++ calls are fire-and-forget; typed calls return futures. Result types
  are scalar, string, tuple, numeric typed-array, packed bitset and list (strings, tuples, typed
  arrays) values; strings and messages are bounded by the buffers policy's message size (64 MiB by
  default). See [ADR-003](adr/ADR-003-protocole-binaire-et-bornes.md).
- `webfront::Task`, `app.spawn` / `ui.spawn` and `TaskHandle` provide owned coroutine lifetime
  and individual cancellation. Typed senders support local cancellation; futures and JavaScript
  Promises do not expose it. Already-dispatched JavaScript continues running. Cancelled calls
  reserve their 16-bit IDs until reply or disconnection; enough outstanding calls exhaust IDs.
  There is no automatic call timeout. See [asynchronous execution](ASYNC_EXECUTION.md).
- HTTP serves one request per socket; WebSocket connections remain open. Accepted stdnet sockets
  enable `TCP_NODELAY`. Gzip fallback has bounded decoding, cache retention and worker admission;
  reads from filesystem providers remain synchronous on the I/O context. See [HTTP assets](HTTP_ASSETS.md).
- The developer environment and `NativeDebugFS` are trusted development facilities, not sandboxes.
  Manifest permissions bound only the scripts' `context` API. Only browser packages install;
  installation scripts and native add-ons do not run. The [content policy](CONTENT_POLICY.md)
  restricts browser origins and navigation. The optional [TLS listener](TLS.md) adds HTTPS and WSS
  with TLS 1.3 and ChaCha20-Poly1305. Each listener admits 16 concurrent handshakes on the I/O thread.
  Client authentication remains an application responsibility.
- `fs::NativeStorage` confines writable storage to a declared root while resolving each path, but
  on POSIX it cannot prevent the effective user or the superuser from moving a directory out of the
  root during an operation: it detects such a move before a commit and after each operation, at
  best. Windows prevents those moves while an operation holds its directories; links are always
  refused there. A `shared` root lifts the POSIX ownership checks. No service uses the storage yet
  (ADR-020). See [ADR-002](adr/ADR-002-systemes-de-fichiers-composables.md).
- A synchronous infinite loop in a spec blocks the page hosting `wf test`; the native test
  driver closes its window natively when its time limit elapses and reports the timeout. Windows file reads beyond 260
  characters remain unsupported (#349).
- Native windows require their platform event loop; WKWebView, WebKitGTK and CEF also require the main thread, while
  WebView2 runs on any thread that is not already MTA (`webfront::UIThread`). Hidden WKWebView tests
  require macOS 14+ and a WindowServer session. Automated CEF coverage is Linux/Xvfb;
  Windows/macOS CEF deployment is not qualified. WebView2 needs its installed Evergreen runtime;
  fixed-version runtime packaging, application installers and accessibility remain application work.
- `WebView2Frontend::open()` releases the STA it initialized. When a window is closed while the runtime is still
  starting, or by a page that closes it on a load failure, the WebView2 runtime (observed with 157.0.4341.46) can keep a
  COM initialization of its own: the thread is then still in an STA (apartment type 3, qualifier 0) when `open()` returns
  (#564, #638). Measured on the Windows CI runner under load, over repeated `show()` calls of the
  `WebView2Lifecycle-repeated-show` case (the repetition and its apartment log were temporary): a page whose bridge
  connection is rejected and that closes the window (`rejected-ready.html`) left an STA in 2 of 600 calls, then in 1 of 1031
  with `open()` dispatching the messages queued on the thread before it released its apartment, so that drain does not
  remove it and was not kept; a page that closes its window (the encoded-path page) and a page that ran to its end and
  closed it (`no-bridge.html`) left none in more than 1600 calls each. That is 3 cases in about 1600 calls after a load
  failure, of the order of one in 500; the cause is not established. In the one case where another `show()` followed on the
  same thread, the remainder was gone after that `show()`, so it can be a delayed release rather than a permanent one; one
  case does not prove it. Do not rely on the thread being out of its STA immediately after `open()` returns. The test
  checks the apartment only after a page that ran to its end.
- The low-level DOM batch command and optional embedded interpreter are implemented; see
  [the wire contract](adr/ADR-003-protocole-binaire-et-bornes.md#commande-dom-dombatch-code-5-version-1).
  The per-connection document/node API, events (callbacks or `co_await node.next(name)`) and grouped
  sender reads are available; see
  [DOM from C++](DOM.md). A direct engine transport remains future work.
  The optional [React adapter](REACT.md) provides a coalesced C++ store and a StrictMode-safe
  `Island`, demonstrated in the Life variant. Its npm archive is prepared locally; public
  registry publication is a separate action. Solid, Svelte, Vue and Lit adapters remain future work.
  Optional [forms and inspectors](FORMS.md) are generated from C++23 type descriptors, which also
  produce TypeScript declarations for `wf check`; an optional C++26 layer reflects the descriptors
  from annotations and is compiled and tested only with GCC 16 (`-std=c++26 -freflection`).
  [ADR-018](adr/ADR-018-acces-au-dom.md) describes DOM islands;
  [ADR-017](adr/ADR-017-acces-direct-aux-frontends.md) retains transport experiments and acceptance criteria.
