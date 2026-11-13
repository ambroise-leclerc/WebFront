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
- **Developer environment.** `NativeApp::bootstrap("developer")` loads the verified, pinned
  environment selected by `DeveloperReleaseData.hpp`. Its terminal provides `wf init`, `create`,
  `install`, `restore`, `run`, `exec`, `check`, `test`, `build`, `dev`, `preview`, `embed`, `list` and `help`. The manifest and lock describe
  dependencies, entries, providers, scripts and permissions. TypeScript/TSX compilation uses
  esbuild-wasm; type checking uses the project's locked TypeScript. Application commands use
  `webfront::Commands`. From environment 0.9.1, a project that has a preview opens
  on its interface over the whole window, with the terminal in a floating window behind a tab on the window's edge. See the
  [developer reference](DEVELOPER_ENVIRONMENT.md).
- **Distribution.** `wf embed` generates the application's compressed C++ provider.
  `NativeAppWithAssets<Provider>` runs without project files, developer callbacks or tool cache.
  Native-engine deployment remains application work. The [README walkthrough](../README.md)
  covers the console, developer and embedded Game of Life stages; Hello World and the host loop
  (`examples/host-loop/`) are the other public examples.
- **Developer automation (opt-in).** `webfront-agent` provides local stdio MCP and a JSON CLI
  for an explicitly launched developer window. Native grants select check/build/test/preview,
  bounded inspection and semantic actions. [Automation](AUTOMATION.md) documents provisioning,
  lifecycle, limits and the supported MCP profile. It requires a compatible environment
  (0.6.3 or later), supplied as a rebuilt archive or verified published distribution. Explicitly shared human windows support authenticated
  native attachment. This does not discover arbitrary processes or isolate untrusted project code.
  An application opts in to being driven with `automation::enableAutomation` (its own window and callbacks), shared sessions are
  listed by `webfront-agent --list`, and `--print-mcp-config` generates the client configuration. The owner may allow-list application callbacks that an agent reaches through the `call` operation, and the terminal, `wf help` and
  the starter's `AGENTS.md` announce automation (environment 0.6.6). Environment 0.6.6 is published; the release pin selects 0.9.2. First-contact guidance for agents (README section, `docs/DEVELOPING_WITH_AGENTS.md`, `webfront_write_agent_guide()`, configuration and startup notices) comes with the library; the `app` starter, `wf help agents` and the errors that give the next steps come with environment 0.9.2 (#561).
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
The suite includes deterministic Catch2 regressions, CMake consumer/configuration checks,
asset freshness checks and, with an enabled engine, real-browser Jasmine and lifecycle tests.
`Journey*` builds a UI with the rebuilt environment and local registry, then compiles and runs
its generated provider from an empty directory. Configured tests need no public network.

[Tiers](TEST_TIERS.md) defines the desktop and extended CI scope, including the GCC 13 Linux aarch64 job (#548).
[Test execution and profiling](TEST_PERFORMANCE.md) documents session isolation, fixture locks,
resource limits and complete-suite repetition. Sessions retain individual CTest assertions;
independent driver processes use OS-assigned loopback ports. Manual transport measurements
remain separate from the short native transport contract tests.

The [#542 investigation](validation/ci-flakes-542.md) reproduces and fixes Windows developer publication held open by
an HTTP reader and concurrent fallback logger delivery. Its qualification passes 20 selected browser graphs on each
native engine and 1,000 repetitions of each Linux closure case. The historical Linux internal stall was not reproduced;
the report distinguishes that evidence from proven causes. The Windows early-close COM lifetime failure remains in
[#564](https://github.com/ambroise-leclerc/WebFront/issues/564), and still makes the complete Windows qualification red.
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
  restricts browser origins and navigation but provides neither TLS nor client authentication.
- `fs::NativeStorage` confines writable storage to a declared root while resolving each path, but
  on POSIX it cannot prevent the effective user or the superuser from moving a directory out of the
  root during an operation: it detects such a move before a commit and after each operation, at
  best. Windows prevents those moves while an operation holds its directories; links are always
  refused there. A `shared` root lifts the POSIX ownership checks. No service uses the storage yet
  (ADR-020). See [ADR-002](adr/ADR-002-systemes-de-fichiers-composables.md).
- A synchronous infinite loop in a spec blocks the page hosting `wf test`; the native test
  driver's watchdog terminates stalled runs with failure. Windows file reads beyond 260
  characters remain unsupported (#349).
- Native windows require their platform event loop and the main thread. Hidden WKWebView tests
  require macOS 14+ and a WindowServer session. Automated CEF coverage is Linux/Xvfb;
  Windows/macOS CEF deployment is not qualified. WebView2 needs its installed Evergreen runtime;
  fixed-version runtime packaging, application installers and accessibility remain application work.
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
