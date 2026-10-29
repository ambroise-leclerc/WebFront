# WebFront project status

Release history belongs in the [changelog](CHANGELOG.md)
and Git; architecture decisions and their evidence are indexed in [the ADRs](adr/README.md).

## Supported paths

WebFront is a header-only C++23 HTTP/WebSocket core plus a standalone developer environment.

- **C++ applications.** Link `WebFront::WebFront` from a CMake parent project. The default
  `webfront::WebFront` uses the system browser. Native applications explicitly select WebView2,
  WKWebView, WebKitGTK or CEF. Standalone and consumer builds detect the platform's native
  engine when its dependencies are available (outside test/fuzzing builds).
  `webfront_use_native_window(target)` provides target setup and a definition for selecting
  the native adapter with a browser fallback; see the [consumer example](../README.md#use-webfront-in-your-project)
  and [frontends](FRONTENDS.md).
- **Developer environment.** `NativeApp::bootstrap("developer")` loads the verified, pinned
  environment selected by `DeveloperReleaseData.hpp`. Its terminal provides `wf init`, `create`, `install`, `restore`, `run`,
  `exec`, `check`, `test`, `build`, `dev`, `preview`, `embed`, `list` and `help`. The manifest and lock describe
  dependencies, entries, providers, scripts and permissions. TypeScript/TSX compilation uses
  esbuild-wasm; type checking uses the project's locked TypeScript. Application commands use
  `webfront::Commands`. See the [developer reference](DEVELOPER_ENVIRONMENT.md).
- **Distribution.** `wf embed` generates the application's compressed C++ provider.
  `NativeAppWithAssets<Provider>` runs without project files, developer callbacks or tool cache.
  Native-engine deployment remains application work. The [README walkthrough](../README.md)
  covers the console, developer and embedded Game of Life stages; Hello World is the other public example.
- **Developer automation (opt-in).** `webfront-agent` provides local stdio MCP and a JSON CLI
  for an explicitly launched developer window. Native grants select check/build/test/preview,
  bounded inspection and semantic actions. [Automation](AUTOMATION.md) documents provisioning,
  lifecycle, limits and the supported MCP profile. It requires a compatible environment (0.6.3 or later),
  supplied as a rebuilt archive or verified published distribution. Explicitly shared human windows support authenticated
  native attachment. This does not discover arbitrary processes or isolate untrusted project code.
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
specs and external process deadlines; it does not compile TS or isolate untrusted application code.

The CEF-off build and complete CTest commands are in [AGENTS.md](../AGENTS.md).
The suite includes deterministic Catch2 regressions, CMake consumer/configuration checks,
asset freshness checks and, with an enabled engine, real-browser Jasmine and lifecycle tests.
`Journey*` builds a UI with the rebuilt environment and local registry, then compiles and runs
its generated provider from an empty directory. Configured tests need no public network.

[Tiers](TEST_TIERS.md) defines the desktop and extended CI scope.
[Test execution and profiling](TEST_PERFORMANCE.md) documents session isolation, fixture locks,
resource limits and complete-suite repetition. Sessions retain individual CTest assertions;
independent driver processes use OS-assigned loopback ports. Manual transport measurements
remain separate from the short native transport contract tests.

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
  are scalar, string, tuple, numeric typed-array and packed bitset values. Strings have a
  16-bit byte length. See [ADR-003](adr/ADR-003-protocole-binaire-et-bornes.md).
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
- A synchronous infinite loop in a spec blocks the page hosting `wf test`; the native test
  driver's watchdog terminates stalled runs with failure. Windows file reads beyond 260
  characters remain unsupported (#349).
- Native windows require their platform event loop and the main thread. Hidden WKWebView tests
  require macOS 14+ and a WindowServer session. Automated CEF coverage is Linux/Xvfb;
  Windows/macOS CEF deployment is not qualified. WebView2 needs its installed Evergreen runtime;
  fixed-version runtime packaging, application installers and accessibility remain application work.
- The low-level DOM batch command and optional embedded interpreter are implemented; see
  [the wire contract](adr/ADR-003-protocole-binaire-et-bornes.md#commande-dom-dombatch-code-5-version-1).
  The per-connection document/node API, events and grouped sender reads are available; see
  [DOM from C++](DOM.md). A direct engine transport remains future work.
  The optional [React adapter](REACT.md) provides a coalesced C++ store and a StrictMode-safe
  `Island`, demonstrated in the Life variant. Its npm archive is prepared locally; public
  registry publication is a separate action. Solid, Svelte, Vue and Lit adapters remain future work.
  [ADR-018](adr/ADR-018-acces-au-dom.md) describes DOM islands;
  [ADR-017](adr/ADR-017-acces-direct-aux-frontends.md) retains transport experiments and acceptance criteria.
