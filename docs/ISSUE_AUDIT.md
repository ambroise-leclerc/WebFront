# Open GitHub issue audit

Audit snapshot: 2026-07-16. These are recommendations only; no GitHub issues were changed.

| Issue | Finding | Recommended action |
|---|---|---|
| #189 Update develop branch | `develop` is the active integration branch, is ahead of `master`, and its latest matrix passed. The issue has no acceptance criteria. | Close as completed or obsolete with branch/CI evidence. |
| #137 MSEdgeWebview2 frontend | Still a distinct possible frontend; a remote feature branch exists, but the parent frontend epic is closed. | Retain only after rewriting scope, supported Windows versions, lifecycle API, and tests; keep outside the minimal milestone. |
| #133 CreateReactApp evaluation | Create React App is no longer an appropriate basis for new React integrations. | Close as obsolete. Create a Vite-specific evaluation only if Node integration becomes a current goal. |
| #132 JavaScript modules | MIME support existed, but no automated module graph and bridge proof existed. | Close when the module demo and browser test in this milestone merge. |
| #125 ReactFS integration | ReactFS, BabelFS, and the named child tasks are implemented; the remaining placeholder task is undefined. | Close as completed; file focused follow-ups for actual missing behavior. |
| #120 Integrated WebFront UI | Broad product idea with no current acceptance criteria. | Rewrite as a concrete roadmap epic or close as inactive. |
| #121 Status/debug overlay | Empty placeholder under #120. | Close unless rewritten with user-visible behavior and frontend constraints. |
| #122 Developer overlay | Empty placeholder and overlaps #121. | Consolidate into a rewritten overlay issue or close. |
| #123 Browser-backed download | Potentially useful but unrelated to the baseline and lacks security/lifetime semantics. | Retain only after defining API, storage ownership, permissions, and tests; otherwise close. |
| #124 Conway readme application | Empty aspirational issue. | Close or rewrite as a documentation/demo task with assets and acceptance criteria. |
| #114 Jasmine automation | Remains valid: Jasmine assets existed, but results were not connected to CTest/CI. | Close when the automated CEF/Xvfb test merges. |
| #115 GruntJS evaluation | Superseded by direct Jasmine plus CEF/Xvfb; Grunt adds no necessary capability. | Close as superseded by #114 implementation. |
| #116 Selenium evaluation | Unneeded for the selected minimal in-process browser path. | Close as superseded; create a new cross-browser issue only if required later. |
| #117 Headless Chrome evaluation | Superseded for the minimal milestone by CEF under Xvfb. | Close as superseded, noting that Playwright may be reconsidered for future cross-browser coverage. |
| #118 Chromatic evaluation | Visual-regression SaaS is unrelated to bridge correctness and the issue is empty. | Close as out of scope. |
| #119 Jasmine Headless WebKit | Superseded by the selected CEF path and has no acceptance criteria. | Close as superseded. |
| #35 Function return | Return/error propagation is not implemented despite partial message types. | Retain as the canonical epic; define correlation IDs, async result API, errors, timeouts, and compatibility. |
| #43 JSReturnValue | Overlaps the C++-to-JS half of #35. | Consolidate into #35 and close as duplicate after preserving its future-like API considerations. |
| #39 Arrays in calls | Enum values exist, but general/typed array encoding is incomplete. | Retain and rewrite with supported element types, ownership, size limits, round-trip tests, and both directions. |
| #38 FileSystem | Layered native and virtual filesystems are implemented; archived packaging remains only an idea. | Close as completed. Open a separate packaged/archive filesystem issue if still wanted. |
| #36 JS strings to views | Technically possible but exposes reception-buffer lifetime hazards. | Retain only as an explicit zero-copy API design task with lifetime guarantees; otherwise close as unsafe/not planned. |
| #28 UI per browser document | `BasicUI` is created per connected `WebLink` and supports script/function interaction; DOM ambitions are undefined. | Close the implemented issue and create a separate DOM API epic only if still desired. |

## Closed issue consistency note

Issue #187 (“Add Jasmine C++/JS bridge testing to JasmineTest”) was closed even though its assertions required unsupported synchronous return values and the executable was not connected to CTest results. The new automation should be referenced in any closing comment for #114 so the historical distinction is clear.
