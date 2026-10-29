# Automation acceptance and distribution candidate — 2026-10-29

This closes the evidence review for ADR-019's implemented local contract. It supplements the
[five-model qualification](automation-models-2026-10-29.md); it does not replace that experiment
or rerun inference. The source is the merged PR #407, `22f67954608924b174ef05dba71a2bc45b392a22`
(final PR head `4a1c92b9a1a5216096a0db99ddd33865ab1bac1a`).

## Evidence reviewed

- #405 provides the comparison and threat model; #406 implements consumer Jasmine/CTest independently
  of MCP. #402 is closed. #407 implements the native controller, CLI, stdio MCP and shared window.
- The final PR head passed the [three desktop jobs](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925937329),
  [CEF GCC/Clang and ASan/UBSan/TSan](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925952697)
  and [developer package checks](https://github.com/ambroise-leclerc/WebFront/actions/runs/40925937495).
- Local acceptance on macOS 27.2 arm64, Clang 23: **357/357 CTest without CEF** and **121/121 developer
  Jasmine specs**. No warning policy was relaxed. These counts include the final entry-label and
  nested dev-script regressions added after the qualification report's earlier 355/118 counts.
- A freshly built WKWebView launcher and driver ran the full `AutomationClient.py --scenario session`
  against the 0.6.3 candidate: diagnostics/correction, check/build/test, preview/actions, cancellation,
  two authenticated attached clients, CLI reuse, SIGTERM and endpoint/profile cleanup.
- `AutomationSdkClient.py`, with the already installed official Python SDK **mcp 2.3.0**, verified
  owned-window and authenticated shared-window use against that same candidate. No SDK was installed
  by configure, the launcher or the test.

## Provenance of the measured tree

The automation sources are those of `22f6795`. The 0.6.3 candidate adds only the release preparation
of draft PR #413 (`01a7906`), whose difference from `22f6795` is limited to `bundles.json`, the generated
`DeveloperReleaseData.hpp` pin and documentation. The samples and the candidate checks were taken from that
combination on a local checkout; the exact tree of those runs was not recorded beyond this description.

## Lifecycle samples

Three fresh generated fixture projects and three native MCP processes were measured with WKWebView.
The fixture's locked packages were installed from the existing verified local test registry before
starting each timer. The environment was the local 0.6.3 archive below, supplied with its SHA-512.
No public network or inference server was used. WKWebView's shared store and OS/browser caches
were not purged: these are not cold-engine benchmarks or latency guarantees.

The measurement used the maintained `Client` in `test/tools/AutomationClient.py` and Python's
`time.monotonic()`. Startup begins immediately before Client constructs Popen and ends after
initialize/notifications and native session polling first observe ready. The operation interval
runs real check and the fixture's passing test and ends after both final results are observed.
Shutdown begins before closing MCP stdin and ends after the native process exits and the client's
reader joins. All runs returned exit 0. The fixture preparation is excluded from all three intervals.

| Sample | Native launch to ready (ms) | Check and passing test (ms) | Stdin close to process exit (ms) |
|---|---:|---:|---:|
| 1 | 787.28 | 712.10 | 74.39 |
| 2 | 776.44 | 760.18 | 78.35 |
| 3 | 789.66 | 755.38 | 66.98 |
| Median | 787.28 | 755.38 | 74.39 |

[Machine-readable samples](automation-acceptance-2026-10-29.json) retain source, frontend and
archive integrity. Four MCP tools serialize to 1,872 UTF-8 bytes as a compact JSON array, or 1,882
bytes inside `{"tools": ...}`. No token count is inferred from this byte count; the model-specific
catalogue projection/token measurements remain in the model report.

## Developer distribution 0.6.3

The locked `wf` project was restored through the local registry and built twice in separate output
directories with `wf run build build-distribution` and `wf run build build-distribution-repeat`.
The environment, Life starter, provision and generated pin match byte for byte. SHA-512 of the
archive independently matches provision.json. The runtime exports connectAutomation and the
real-window tests above establish compatibility rather than relying on a text marker.

| File | Bytes | SHA-256 |
|---|---:|---|
| environment.tgz | 4095568 | fbf4c4036bd084cf4057d4dbc163cabef6444e002e1684896c4be579ad01337a |
| life-starter.tgz | 25497 | 1a79a9d6660a724ed981c17acba5091eda290301279cdf2916414db391ec9e92 |
| provision.json | 387 | cd985f6db306ee41834369322cc348b976292938bb5f0ff21425dd290d2d2539 |
| DeveloperReleaseData.hpp | 541 | 4feca3d4e06dcc6e81f5ac1c9d7750f733ee96fb31d40f8998956e20c5e621a6 |

Environment integrity:

```text
sha512-3AVF2xKsUfHys5VREA6DmkjiEDKjx9VQtPTrsjdiazzfpNQvbKEjW2ox5Kd2hPeOP5MfcMvbDHQ4SZoO97P5Xg==
```

This is a **prepared candidate, not a publication claim**. The canonical 0.6.3 provision URL returned
HTTP 404 during preparation. Pages uses gh-pages at its root. The generated release pin must remain
on its release branch until tagged publication, deployed-byte/CORS verification and the live-engine
smoke check described in [PUBLISHING](../../js/developer/tools/PUBLISHING.md). The accepted ADR and
epic closure concern the delivered source contract; a distribution candidate does not silently
change existing applications' default download. CMake's test-bootstrap pin remains independent.
