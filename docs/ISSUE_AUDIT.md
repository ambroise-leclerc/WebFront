# GitHub issue and branch audit

Audit applied: 2026-07-19. The backlog was reduced to work that has concrete interfaces, acceptance criteria, and a place in the current WebFront roadmap.

## Active issues

| Issue | Decision | Next action |
|---|---|---|
| #39 Typed arrays in bridge messages | Active, first priority. The wire enum already reserves numeric array types, but their codecs are incomplete and the readable browser bridge source was removed in 2023. | Restore a reproducible `WebFront.js` source/embedding pipeline, implement owning numeric arrays in both directions, and cover them in Catch2 and Jasmine. |
| #35 Asynchronous bridge results and errors | Active, second priority. Existing calls remain fire-and-forget even though a partial return-message type exists. | Add correlation identifiers, C++ futures, JavaScript promises, exception propagation, missing-function errors, and disconnect cleanup after #39. |
| #137 Optional WebView2 frontend | Retained but deferred. Closed PR #175 was untested and targeted the example rather than the current frontend abstraction. | Implement a fresh opt-in `frontend::WebView2` only with native Windows compilation and lifecycle/bridge testing. |

Issue #199 tracks this one-time repository cleanup and closes with its documentation PR.

## Closed issues

| Issues | Resolution |
|---|---|
| #189 | Completed by #190 and the subsequent merged foundation work on `develop`. |
| #114–#119 | #114 was completed by #197; the Grunt, Selenium, Chrome, Chromatic, and WebKit evaluations are superseded by the CEF/Xvfb Jasmine integration. |
| #120–#124 | The integrated overlays, browser-backed download, and showcase application lack requirements or fall outside the minimal bridge foundation. |
| #125, #133 | The defined ReactFS work and optional React example exist; Create React App integration is not part of the core roadmap. |
| #132 | Completed by the native ES-module demonstration in #196. |
| #28 | A `BasicUI` already represents each connected `WebLink`; general DOM abstraction is outside the current scope. |
| #38 | The core `File`, `IndexFS`, `NativeDebugFS`, and `Multi` filesystem layers are implemented. |
| #36 | Reception-buffer-backed string views and pointers would expose unsafe lifetimes; incoming values remain owning types. |
| #43 | Consolidated into the canonical return-value issue #35. |

## Branch cleanup

Only protected `develop` and `master` remain as long-lived remote branches. The following remote branches were removed after comparing their tips and PR/issue history:

- `137-msedgewebview2-frontend` and `140-ensure-typeerasedfunction-forwarding-constructor-has-constraints-that-prevent-copying-moving-objects-of-the-same-type` pointed at stale, unrelated history.
- `179-cef-frontend`, `180-cef-crashes-on-macos-during-window-creation-exc_bad_access`, and `182-cef-frontend-for-windows` were superseded by the merged CEF/frontend implementation.
- `claude/resolve-issue-40-01BBmQwYHN1GkxBbzsPXXgmb` was superseded by merged PR #194.
- `feat/windows-webview2` belonged to closed, unmerged, untested PR #175; its requirements were preserved in rewritten issue #137.

Merged and abandoned local feature branches were also removed. New work starts from an up-to-date `develop`, follows the issue-number branch convention, and targets `develop` with one focused pull request per milestone.
