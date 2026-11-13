# Intermittent CI failures (#542)

Investigation on 2026-11-13, starting from `develop` at `f0751433`. Distinguish an assertion failure, a process that
does not close, a runner deadline and an infrastructure failure; a successful rerun does not identify a cause.

## Historical evidence

| Case | Evidence and disposition |
| --- | --- |
| Finished `wf test` directory, Windows rename failures, Python measurement deadlines | Already addressed by #549. Preserve its regressions and the absence of automatic CI retries. |
| Automation watchdog, `AutomationDiscoveryTests.cpp:385` | [Run 42609482904](https://github.com/ambroise-leclerc/WebFront/actions/runs/42609482904), job 129133924123, fails while waiting for `discovery::list(sessions).empty()`, after disconnection. Run 42611883578 fails the same withdrawal in the next section. These precede commits `6f1ff05f` and `46c42f06` (#501/#506), which retry removal of a discovery record held open by a Windows reader. They do not establish a missed operation deadline. |
| macOS file-services and browser suite | The historical failures are the 5-second file-services deadline and an unrelated queued video `seeked` at position zero. #504 already adds per-stage diagnostics, a shared bounded spec budget and position-aware event acceptance. Its investigation and 50-pass qualification are in [wkwebview-specs-504.md](wkwebview-specs-504.md). |
| Linux navigation allow-list | [Run 42989857086](https://github.com/ambroise-leclerc/WebFront/actions/runs/42989857086), job 130421485226, reaches the 90-second outer deadline with only a GTK session-bus warning. That warning alone does not identify the stall. The current lifecycle runner has a native stage watchdog; qualification must retain its output. |
| Linux embedded Life | [Run 42814532103](https://github.com/ambroise-leclerc/WebFront/actions/runs/42814532103), job 129824212032, prints the successful UI assertion before the 60-second process deadline. This is a closure/shutdown investigation, not evidence of a failed Life assertion. |
| Windows missing scripts/styles and `Failed to fetch` | Historical driver and consumer logs lack request diagnostics. Extend the existing developer-session HTTP logging to both paths and test builds of the agent. Keep HTTP errors as well as request/status lines, without bridge chatter or secret queries. |
| Windows workspace reload | [Run 43383593547, attempt 2](https://github.com/ambroise-leclerc/WebFront/actions/runs/43383593547/attempts/2), job 131758702589, answers the preview resources with HTTP 200; its only 404 is the deliberately missing registry package. This does not prove script execution or establish a refused file. The assertion now retains the preview URL, document state, root, tab state and complete `wf dev` output. |
| Timing analysis | Attempt 3 of that run, job 131761136790, contains `durationMs=4723.50020276.900`. Independent developer servers interleaved multi-part output. Serialize their progress/result lines; reject the complete malformed numeric token in the optional analyzer, warn and retain valid phases. The regression reproduces the former `ValueError`; the fixed CLI reads the original artifact and retains 303 valid phases. |
| Windows COM early closure | Tracked separately in #564. Do not count it as a resource-loading fix. |

## Changes and regression coverage

- Driver cases and nested Jasmine runners occupy the native viewport. The old off-screen driver case stalls the
  WebSuite fixture graph locally; the visible case completes it in 2.73 seconds. A case checks its viewport, without
  awaiting an animation callback that can stop when the window becomes inactive. Preserve the runner title used by
  the Jasmine readiness handshake. Each grouped driver reserves four CTest processors.
- The Linux interface loop owns one eventfd watch. Producer threads signal the kernel counter instead of allocating
  GLib idle sources. Removal happens on the interface thread after outstanding wake/stop calls return. The old broad
  GLib ThreadSanitizer suppression is removed; a four-producer counter regression checks delivery and reset.
- Automation tests use an injected steady-clock source for exact startup/operation/grace boundaries and retain a
  real-clock case. Startup readiness and expiry share one session lock. The close grace starts before sending the
  browser close request, so dispatch time cannot extend it.
- Capture and executor tests wait for observable file/result/attachment/closure states. The streamed-reply shutdown
  test waits until its deferred send registers cancellation, rather than merely observing bytes prepared before
  sender start. TSan exposed that race in run 43411600509; it is independent of the GLib report.
- The optional manual qualification repeats the complete fixture graph three times, then the independent browser
  cases 20 times and the TSan interface-loop case 100 times. `until-fail` stops on a failure; there is no pass-on-retry.
  Installed fixtures are retained for independent-process stress tests; stateful setup is not repeated alone.

## Validation record

Local platform: macOS arm64, AppleClang 21, Release, CEF off, WKWebView on, four CTest workers. The diagnostics-only
revision passes 724/724 cases in three complete fixture graphs. File-services also passes 60 independent repetitions.
The revised clock/frame/send changes pass another complete three-pass invocation: 726/726 in each pass,
130.88/119.03/119.64 seconds. The selected stress graph also passes locally (13 cases, 31.29 seconds).

The first manual workflow, [43404654087](https://github.com/ambroise-leclerc/WebFront/actions/runs/43404654087), passes
the Linux/macOS three-pass suites, GCC/Clang CEF and both sanitizers on the diagnostics revision. Windows reaches its
25-minute job limit after a 21-minute cold MSVC analysis build; the explicit manual three-pass job now has 60 minutes.
Individual case deadlines remain unchanged.

The next workflow, [43411600509](https://github.com/ambroise-leclerc/WebFront/actions/runs/43411600509), exposes a
runner-title regression on all engines and the deferred-send registration race under TSan. Both are corrected;
that failed revision is retained as evidence and cannot qualify the final changes. The final multi-platform campaign
must pass before treating the unresolved platform cases as qualified or creating the implementation PR.
