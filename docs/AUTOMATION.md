# Developer automation and local MCP

WebFront's optional `webfront-agent` opens a developer window and exposes a bounded native
controller through either a JSON CLI operation or MCP on stdio. The terminal remains available
in the window. Both interfaces use the same wf dispatcher and reject overlapping commands.
Normal applications and embedded distributions acquire no automation callbacks or listener.

The launcher can own a window or attach to a human-owned developer window explicitly opened for
sharing. It does not discover arbitrary running applications, open an HTTP endpoint, or export the
bridge as MCP. Applications can explicitly integrate `automation::Binding` with their own controller.
A session ID routes operations and prevents accidental cross-session requests; it is not an
authentication secret. The opt-in native attachment channel uses a separate OS-generated capability.

For testing a consumer's own page through CTest, see [consumer testing](CONSUMER_TESTING.md).
That independent API is available in this tree; MCP is unnecessary for CI to run Jasmine.

## Building and provisioning

Enable an embedded frontend and the launcher. For example, on macOS:

```sh
cmake -S . -B build-agent -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_WKWEBVIEW=ON -DWEBFRONT_BUILD_AUTOMATION=ON -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build-agent --target webfront-agent webfront-wf --parallel 2
```

Select WebView2 on Windows or CEF/WebKitGTK on Linux with their existing CMake options.
`WEBFRONT_AUTOMATION_FRONTEND` chooses the enabled engine when several are available. Browser
test configurations also build the launcher for their regression tests. Without an enabled
embedded engine, requesting the launcher fails configure instead of opening a system browser.
The implementation is native C++23; no Node.js, npm or Python is required to run the launcher.

Provision an environment containing automation API v1 (0.6.3 or later). The setup operations (`init`, `create`, `install`,
`restore`, `embed`) need environment **0.6.4 or later**: the launcher checks this at startup, and granting one of them to an
older environment fails with a bootstrap error (status 70) naming the operation, instead of failing each run. Environment 0.6.2
predates that API; passing an older environment produces a bootstrap error with status 70.
The launcher takes an explicit archive and its independently trusted SHA-512 SRI. Use a verified
published distribution or build one in this repository:

```sh
build-agent/test/web/webfront-wf --project "$PWD" --manifest-dir js/developer -- restore
build-agent/test/web/webfront-wf --project "$PWD" --manifest-dir js/developer -- run build build-distribution
```

`build-distribution/provision.json` gives the resulting integrity. Project packages must also be
installed/restored beforehand through wf, or by an agent granted `install`/`restore`. Tests use configure-provisioned packages and a local registry, with no public network
after provisioning. This source build does not publish a release or change the pinned bootstrap.

## Invocation and authority

`webfront-agent --schema` prints the trusted JSON Schema contract v1 without opening a browser.
`webfront-agent --agent-help` prints small, on-demand instructions for CLI agents, including local
models that do not need a tool catalogue in every prompt. `automation/Contract.hpp` contains the
single versioned contract for argument definitions, per-operation permissions/required fields,
result/error vocabulary and limits. Native validation, CLI argument discovery and MCP's start
schema project that contract; transport choice cannot grant additional operations. The native
validator enforces the documented schema subset plus path containment and byte limits.

```sh
webfront-agent --project /absolute/project \
  --environment /absolute/environment.tgz --integrity sha512-TRUSTED_DIGEST \
  --allow check --allow build --allow test --allow preview --allow inspect --allow interact --mcp
```

All executable operations are denied unless granted by the process owner using `--allow`.
Valid grants are `check`, `build`, `test`, `preview`, `inspect`, `interact` (click/fill) and the project setup
grants `init`, `create`, `install`, `restore` and `embed`. The setup grants write the manifest, the lock, the cache, a new
starter directory or the generated provider header; grant them only for a project whose registry you trust.
The project, environment, manifest directory and grants cannot be changed by MCP or the manifest.
Omit grants to expose session discovery only. In particular, there is no agent tool for shell,
JavaScript evaluation, arbitrary callback calls, `wf run`, `exec` or arbitrary
native file reads/writes. `install` and `restore` use the registry already configured by the owner, the manifest or the
default; the operation cannot choose another one. An agent edits project files through its separately authorized workspace
tools, then asks WebFront to verify them.

`--manifest-dir` selects a project-relative manifest directory. `--registry-dir` serves an
explicit local registry for bootstrap/project tooling. `--timeout` sets the native per-operation limit and `--startup-timeout` separately bounds
bootstrap (both default to 120 seconds, 1–3600); progress does not extend either deadline. Each process owns
an automation session, but browser storage isolation depends on the frontend:

| Frontend | Browser storage used by the launcher |
| --- | --- |
| CEF on Linux | Separate temporary profile directory |
| WebView2 on Windows | Separate temporary user-data directory |
| WKWebView on macOS | Engine's default shared website data store; no per-process storage isolation |
| WebKitGTK on Linux | Default WebFront XDG data/cache directories; shared between launches |

The launcher's temporary directory is used as a profile only by CEF/WebView2. Cleanup is attempted
on normal and watchdog exits; this does not erase WKWebView/WebKitGTK's shared stores. An independent
process is not a browser-storage or hostile-code sandbox. Use `xvfb-run -a` on headless Linux.

## Sharing a window already open for a human

The human first launches the window with a new private endpoint directory and explicit grants:

```sh
webfront-agent --project /absolute/project \
  --environment /absolute/environment.tgz --integrity sha512-TRUSTED_DIGEST \
  --allow check --allow build --allow test --allow preview --allow inspect --allow interact \
  --serve --listen /absolute/private-session
```

The window and its terminal stay open independently of an agent. To connect later:

```sh
webfront-agent --attach /absolute/private-session --mcp
webfront-agent --attach /absolute/private-session --operation check
webfront-agent --attach /absolute/private-session --operation test --pattern passing
```

The attached CLI and MCP clients operate on that same native controller and existing window.
They cannot replace the project or change its grants. Multiple attached clients share the run
history and busy/cancel rules. Closing an attached client's stdin detaches it and leaves the human
window open. The owner can close its window normally, or explicitly run
`webfront-agent --attach /absolute/private-session --shutdown` to revoke the session and close it.

Native attachment is an opt-in JSON-line channel over an OS-assigned **127.0.0.1-only TCP port**,
not MCP over HTTP/WebSocket. The endpoint directory must be new and outside the project, so the developer file bridge cannot read its capability. POSIX creates it atomically with
mode 0700 and the endpoint file with mode 0600; Windows uses an owner-only inheritable ACL.
On POSIX the client opens the directory with `O_DIRECTORY|O_NOFOLLOW`, checks its uid and private
mode, then opens `endpoint.json` relative to that pinned descriptor with `openat` and verifies the
file's uid, regular-file type and private mode. A pathname swap cannot redirect that read. The Windows reader rejects reparse points but does not independently verify the
file's owner or ACL: it relies on the private directory ACL established by the owner. Only attach
to an endpoint in a trusted location; do not substitute a copied or externally created directory.
Session IDs also use OS entropy and constant-time comparison; start authorization precedes
operation/argument validation and refusals enter the authorization audit.
Its `endpoint.json` holds the port and a 256-bit capability generated by the OS. The capability is
never placed in page content, URLs, command-line arguments or logs. Each native request must prove
it before any operation executes. Web requests and incorrect capabilities are refused. Do not copy
this file to a shared location. The server removes its file/directory on normal closure and catchable SIGINT/SIGTERM, without
recursively deleting caller-owned content. Independent processes without a shared endpoint file
remain inaccessible to the attachment client. Catchable interrupts also remove the temporary
native profile. The CEF launcher disables ContentMain signal handlers and restores the native SIGINT/SIGTERM
actions after browser initialization; Windows retries transient browser file locks for up to two seconds. SIGKILL, forced OS termination and power loss cannot run cleanup. Same-user compromise remains outside this boundary.
Unauthenticated peers are multiplexed in a bounded queue (64, with oldest-peer eviction and a
two-second handshake deadline); they do not consume the 32 authenticated request worker slots.
This prevents silent connections from exhausting that worker pool. A sustained local flood can
still deny service through OS or CPU resource exhaustion. Attached clients wait up to 30 seconds
for each response; `--request-timeout 1..3600` changes this independently of the operation and
startup deadlines. CLI result polling backs off from 50 ms to one second. There is no TLS or remote/multi-user authentication promise.
Listening, accepted and client sockets are non-inheritable; POSIX closes them on exec. The page
capability uses the same OS cryptographic generator and comparison as the attachment channel.

Stdout contains only one JSON-RPC message per line in MCP mode. Diagnostics go to stderr.
The client owns the stdio adapter and closes stdin to end its connection. In owned-window mode
this also closes the window; attached mode leaves its owner running. No native attachment socket
is created unless `--listen` is explicit. A blocked renderer is revoked at its native deadline; the
supervisor requests window closure, then exits forcibly after a five-second grace period. It first
stops the attachment worker, closes its listener and removes the endpoint files and empty private
directory. Active attachment IO may take up to its two-second deadline to join. Once a session
closes, its listener is revoked instead of accepting clients that nobody will serve. Attached CLI
startup discovery backs off from 100 ms to one second, rather than opening a connection every 20 ms.

For a single CLI operation, replace `--mcp` with `--operation build [--entry main]` or
`--operation test [--pattern text]`. Check, build, test, preview and inspect use the same
controller and permissions. The CLI waits, emits one final JSON run snapshot and returns 0
on success, 1 on operation failure, 64 on configuration errors, 69 when an attachment is unavailable
(the outcome of an in-flight mutation may be unknown), 70 on bootstrap failure,
71 on the native deadline. Bootstrap failure stays 70 whether the transport observes the closed
session before or after the main thread; specific native deadline and signal statuses retain priority.
Closing a window without a command result does not imply success.
MCP operation failures are returned as tool errors or failed run states; clients must inspect
the result instead of interpreting successful JSON-RPC delivery as a successful build.

## MCP profile and results

The server supports protocol revision **2025-11-25**, initialization, ping and tools. It advertises
no prompts, resources, sampling, logging or experimental task capability. A client requesting
another revision receives the supported revision and must decide whether it can continue, as
specified by [MCP version negotiation](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle#version-negotiation).
It should disconnect if it does not support the offered revision; the server does not silently
claim support for the client's unknown version.
Tool results include both `structuredContent` and its JSON spelling as text.

| Tool | Arguments | Result |
| --- | --- | --- |
| `webfront.session` | `{}` | API version, session ID, canonical project, state, native grants, failure reason and bounded audit metadata. |
| `webfront.start` | `{session, operation, arguments?}` | A new run ID and initial state, returned immediately. |
| `webfront.result` | `{session, runId}` | Run state, bounded streamed output and final result. |
| `webfront.cancel` | `{session, runId}` | Cooperative cancellation request and current state. |

Operations and their arguments:

| Operation | Arguments | Grant |
| --- | --- | --- |
| `check` | `{}` | `check` |
| `build`, `preview` | `{entry?}` | Operation's own grant |
| `test` | `{pattern?}` | `test` |
| `inspect` | `{}` | `inspect` |
| `init` | `{name?}` | `init` |
| `create` | `{starter, directory?}` | `create` |
| `install` | `{package?}` (one `name[@range]`; none installs the locked graph) | `install` |
| `restore` | `{}` | `restore` |
| `embed` | `{provider}` or `{entry, output}` | `embed` |
| `click` | `{pageId, elementId}` | `interact` |
| `fill` | `{pageId, elementId, value}` | `interact` |

`name`, `starter`, `package` and `provider` refuse a leading `-`, a backslash and spaces; `directory` and `output` must be
project-relative paths: `create` never replaces an existing directory (`wf` refuses it), while `embed` **replaces** its output header, as
regeneration requires. `embed` needs a provider, or an entry together with an output, never both.
Unknown arguments, wrong sessions, unsupported commands and option-like entry/pattern strings
are refused natively. Entries accept manifest labels (including `app:debug` and `app..debug`);
entry paths must be project-relative. A queued/running operation blocks
another native start. A terminal command already active in the page causes an explicit busy
failure instead of interleaving mutations. A `wf run` alias passes ownership to its resolved
command; module scripts retain their lease through execution and publication.
Interrupting a terminal command lets worker cancellation finish, then
returns control within 2.5 seconds if its Promise stalls, while retaining the lease until all
underlying and nested work actually settles. New operations remain busy during that interval;
reload or close a permanently stuck environment to stop it. `wf test` finishes
its bounded cancellation/cleanup before releasing ownership. A human `wf dev` owns the lease only
while compiling/publishing, including when launched through `wf run` aliases, so agents can use
the window while it waits for edits. The window displays the agent operation and a stop button.

Poll the run ID until its state is `succeeded`, `failed`, `cancelled`, `timeout` or `disconnected`.
Exit code 130 means `cancelled` only after a native cancel request or an authenticated page
report of an actual local abort; an unrequested script exit 130 is `failed`.
The final `result` contains `{exitCode, output, report}`; top-level `output` becomes that final
bounded output instead of the progress tail. Jasmine supplies `report`, while compiler
errors may also provide a bounded `diagnostics` array with message, source file and one-based
line/column. Inspection and actions return `data`. No terminal text parsing is needed to determine
success or locate the structured diagnostics. Output is bounded in memory and on the bridge;
`truncated` records a cut. Only the latest 64 runs are retained in process memory. An expired ID
is reported explicitly and is never silently re-executed.

`runNonInteractive` keeps its full returned output by default. Automation explicitly requests a
16,000 UTF-16-unit rolling tail; progress exposes its latest 2,048 units. A noisy command therefore
keeps fresh progress and its final summary. Serialization may reduce the tail further to fit the
bridge byte limit, with `truncated: true`; it does not replace the tail with an old prefix.

`notifications/cancelled` refers to an MCP request. Start requests finish immediately, so those
notifications cannot cancel a later background operation; use `webfront.cancel` with its run ID.
Cancellation is cooperative and cannot undo committed publications or effects in page code.
A timed-out renderer is never reused for another operation. Do not retry a mutation blindly
after a lost response; inspect its run state or report the uncertainty.

Preview inspection is limited to the local developer preview, visible elements with explicit IDs,
bounded text/labels and observed error/rejection events. It omits form values and full HTML.
Click accepts buttons, checkboxes and radio/submit controls; fill accepts ordinary text/number
fields, excluding passwords and file inputs. Actions require the inspected page revision and are
refused after navigation or while loading. Failed navigations cancel provisional loading before
the previous document can be inspected again, with a fresh revision. A navigation still pending
after 30 seconds is stopped; a replacement document already complete can also be recognized without
its load event. Inaccessible or unauthorized documents remain refused. They are synthetic DOM interactions, not a guarantee
of browser-trusted input. Captures, arbitrary selectors and full accessibility trees are deferred
because they broaden disclosure and require engine-specific qualification.

## Security boundary

Launching the server grants the client authority over the explicitly selected trusted project.
The native controller checks its grant on every start. The launcher generates a separate 256-bit
single-use window claim and passes it in the frontend URL fragment, which is not sent in HTTP
requests. The bootstrap removes the fragment from browser history before submitting its claim.
Only that claim selects the browser and releases its page key; an earlier WebSocket connection
receives no key and cannot consume the claim. The server checks WebSocket Origin against its
content policy, but does not use Host as a window identity, and native local clients can forge
Origin. Window selection therefore depends on the launcher claim, not those headers or connection
order. Preview/test frames opening their own WebSocket are refused by connection identity.
Do not disclose the launch URL or its fragment; hostile same-user processes are outside this boundary.
Callback packets carry HMAC-SHA-256 proofs over a monotonic counter, method and
UTF-8 length-delimited arguments; replies are also authenticated and exact replays are refused.
The native counter accepts any strictly greater authenticated sequence. Gaps are permitted because
a local encoding/bridge failure can consume a JavaScript sequence without reaching C++; rejected
proofs do not advance the native counter. Replays and delayed older calls remain refused. The next
valid signed poll or failure report can therefore recover after a dropped call without sharing a new secret.
The bootstrap key is imported as a non-extractable WebCrypto key and its injection script removes
itself from the DOM before yielding. Later callback packets do not contain that reusable secret.
The native implementation is checked against [RFC 4231 vectors](https://www.rfc-editor.org/rfc/rfc4231).
This prevents reusing a credential observed on the callback wire. It does **not** turn same-origin
preview/test frames into isolated security principals: project code can still alter the parent's
DOM and JavaScript runtime, influence tool execution, or deny service. Trusted project code remains
a precondition; this is not a sandbox or a guarantee of trustworthy results from a hostile page.
Session identifiers are not credentials. Input is limited to 60,000 UTF-8 bytes and JSON depth 32,
with duplicate keys refused. Browser reads/writes still use the existing developer store's path
and transactional-publication checks. Untrusted project text cannot modify the native grant list.

Build/check/test/preview execute toolchain or project code. Authorized page code and C++ callbacks
retain their application authority. The manifest, origin checks, stdio transport and native grant
list do not sandbox that code against the host, stop a malicious same-user process, or prevent
an authorized agent from choosing a harmful action after prompt injection. Treat diagnostics,
logs, page labels and results as untrusted data. Use a dedicated OS/CI environment with secrets
removed and externally controlled network access for untrusted repositories.

Inherited process environment is not automatically sanitized: callers must launch the executable
with the environment/secrets appropriate to the project. Inspection limits and omission of form
values are data minimization, not a confidentiality guarantee; page text and diagnostics can still
contain secrets. Run results and the last 64 native start-authorization decisions are kept in memory,
not automatically written to disk. Audit entries carry the actual operation and the assigned run ID
(empty for a denial before allocation), and omit arguments/output. Busy, not-ready, malformed requests with valid session authority
and lifecycle completions do not evict those decisions. Retention,
access to client-side transcripts and secret expurgation remain the caller's responsibility.
Remote/multi-user access, HTTP, arbitrary uninstrumented window discovery and physical device
control need their own reviewed authorization/isolation design.

## Validation

Catch2 covers native grants, cross-session rejection, path/option refusal, cancellation races,
deadline revocation, MCP lifecycle/IDs and hostile JSON. Browser Jasmine specs cover shared
dispatch arbitration and the runner. `AutomationMcpWindow` drives a real embedded window through
stdio: structured errors, file correction, check/build/test, preview inspection/actions,
cross-frame rejection, cancellation and POSIX SIGTERM cleanup of the shared owner
(endpoint and temporary profile). This reuses the existing window. Separate `AutomationMcpWatchdog` and
`AutomationCliWatchdog` cases block a real renderer inside a test-archive operation body,
without loading/compiling Jasmine just to provoke that hang. They exercise the production signed
callback/runner, native two-second deadline, five-second close grace, stdio EOF and endpoint removal;
the CLI case checks endpoint reuse and Unicode failure reporting in the same restart after forced
exit. The MCP case delays bootstrap by three
seconds to verify that its shorter operation deadline starts only after startup. The cases retain
separate private project/temporary directories and ephemeral ports; the frontend storage limits
listed above still apply. Only the two spinning renderers share a lock.
That regression uses Python's standard library as a test client; Python is not a runtime
dependency of the launcher. It is registered when a Python interpreter and embedded test engine
are available; without xvfb-run on Linux only the optional Automation window tests are skipped with
a warning. The mandatory Jasmine, developer and driver browser coverage still requires a display
runner and configuration fails if it is unavailable.
The maintained [official SDK regression](../test/tools/AutomationSdkClient.py) uses `mcp==2.3.0`
in a separate test environment. It checks initialization, catalogue, structured results, an operation
failure, a real build and detachment from a shared window. It accepts the same path arguments as
`AutomationClient.py`. Configure registers `AutomationSdkWindow` when the selected Python already
has that exact SDK installed; no SDK or package is installed by configure or by a test. To run it
manually, use that Python with `test/tools/AutomationSdkClient.py --agent ... --driver ...
--environment ... --provision ... --registry ... --fixture ... --work ...`. This is tested interop,
not a claim of compliance with every MCP client.

### Optional local-model qualification

`test/tools/AutomationModelBenchmark.py` is a small MCP host for an already running local Ollama
model. It does not install or download a model, contact a public server, or grant arbitrary shell
or filesystem access. It projects the trusted MCP catalogue into function calls and separately
permits reading/writing only the generated fixture's `main.ts`. Two tasks ask the model to fix a
type error or a failing test. The harness then independently runs check/tests, records successes
and attempts per model, latency, prompt/generated token counts and the host's measured catalogue
token delta. It measures the catalogue with/without tools using identical baseline messages;
that delta is host/model-specific, not a universal tokenizer estimate.

Run it with the same agent/driver/environment/provision/registry/fixture/work options as
`AutomationClient.py`, plus `--model INSTALLED_MODEL --report results.json`. Repeat `--model` to
compare model classes; `--repeat` defaults to 3 and `--turns` to 24. `--ollama-url` defaults to
`http://127.0.0.1:11434/api/chat`; only loopback HTTP endpoints are accepted and redirects are refused.
The synthetic source produced by a model is still executable project code: use an isolated
environment with no secrets for unfamiliar models. Results are not a general safety or success
guarantee, and an unavailable model is never counted as a passing experiment.

The host follows the [Ollama tool-calling protocol](https://docs.ollama.com/capabilities/tool-calling)
and [chat token counters](https://docs.ollama.com/api/chat), consulted on 2026-10-28.

The [2026-10-29 local-model qualification](validation/automation-models-2026-10-29.md) records
five installed models tested through a separate LM Studio measurement adapter: Devstral Small 2,
Qwen 3.8, Muse Glimmer, Flint and Nemotron Cascade 2. All 30 final sources passed independent host
check/tests; 29 model conversations themselves observed both operations succeed. The report includes
per-task data, catalogue/token costs, recovered refusals, the Flint response-limit case and native
exit-status validation. This is measured evidence for that setup, not a supported LM Studio mode
of the maintained Ollama benchmark or a general success guarantee.
