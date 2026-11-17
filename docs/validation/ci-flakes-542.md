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

- The new Windows diagnostic in run 43414910133, job 131851704974, identifies the missed workspace reload as
  `rename: Access is denied` while moving `preview` to its backup. A held `DeveloperFS` HTTP source reproduces this
  deterministically in [43426274521](https://github.com/ambroise-leclerc/WebFront/actions/runs/43426274521).
  Sharing deletion does not suffice: Windows [forbids renaming a directory containing open files](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information).
  Retain a read-only section and close the file handle before returning the resource; map/unmap only each read's
  window. The response retains its old bytes across replacement without holding the publication directory open or
  mapping the entire asset. [43427838167](https://github.com/ambroise-leclerc/WebFront/actions/runs/43427838167) passes
  that reproduction; [43428301160](https://github.com/ambroise-leclerc/WebFront/actions/runs/43428301160) also verifies
  backup cleanup while sections remain alive and replacement with an empty file. Unit coverage includes a read
  across the mapping granularity, seeking, EOF and simultaneous old/new versions.
- The macOS stress campaign in that run traps in `WebSocket::enqueue` through the fallback log transport.
  The default logger concurrently reads/modifies its callback vector without synchronization, and removal does not
  wait for an active callback. The removal regression fails before the change. A three-thread registration/logging
  probe reports that vector race under local ThreadSanitizer before the change and exits successfully afterwards.
  Snapshot registrations under a mutex, invoke outside it, retire callbacks before releasing captured state, make
  severity flags atomic, and protect transport callbacks with the link's existing lifetime guard.
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
  The remaining unit sleeps were reviewed: polling helpers return as soon as their predicate holds; delayed release
  threads deliberately keep a resource busy; logger/loop/body negative observations check that blocked work has not
  completed; host descendant checks allow the child enough time to create its forbidden marker. These are distinct
  from sleeping and then assuming an asynchronous startup or capture has completed.
- Embedded Life explicitly shuts down before its callback-captured result state and empty working directory leave
  scope. Its two phase messages distinguish a frontend that did not return from a stalled native shutdown; they do
  not claim that the historical timeout's internal cause has been reproduced.
- The optional manual qualification repeats the complete fixture graph three times, then the independent browser
  cases 20 times and the TSan interface-loop case 100 times. `until-fail` stops on a failure; there is no pass-on-retry.
  Installed fixtures are retained for independent-process stress tests; stateful setup is not repeated alone.
- GCC 13 ARM64's sanitizer pass also exposes an assertion made while the descriptor-exhaustion test has no descriptors
  available, and the 128 MiB stream test's 16384 worker round trips exceed its guard. Restore descriptors before any
  Catch2 assertion (also on an exception), and use a 64 KiB test buffer for the same 128 MiB/48 MiB memory proof,
  reducing the handoffs to 2048. Neither the transmitted size nor the memory-growth bound is reduced.

GCC 13's [vptr lookup](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.4.0/libsanitizer/ubsan/ubsan_type_hash_itanium.cpp#L181)
uses [an accessibility check implemented with a pipe](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.4.0/libsanitizer/sanitizer_common/sanitizer_posix_libcdep.cpp#L250).
If the two pipe descriptors cannot be allocated, it returns false. This explains why an uncached Catch2 virtual-type
check can report an invalid vptr under deliberate descriptor exhaustion. The test now retains the exception and
inspects it only after releasing pressure; sanitizer checks remain enabled.
The ordinary missing-file error is exercised before fault injection as well, so GCC 13 can perform the initial
virtual-type checks while their own pipe can be allocated. The isolated native ARM64 probe in
[43428433115](https://github.com/ambroise-leclerc/WebFront/actions/runs/43428433115) reproduces the cold constructor's
invalid-vptr report and then passes error/recovery with that preparation, with ASan and UBSan still enabled.

The optional mddlog qualification also exposed an outdated context assertion: it still expected a binary dump,
although `onBinaryMessage` has intentionally omitted payload bytes since `59d5aaaa` (2026-11-04). Keep that redaction:
the regression now requires the message summary and call context, and verifies the test payload is absent from both
the structured message and rendered text. No payload logging is restored to satisfy the old assertion.

## Validation record

Local platform: macOS arm64, AppleClang 21, Release, CEF off, WKWebView on, four CTest workers. The diagnostics-only
revision passes 724/724 cases in three complete fixture graphs. File-services also passes 60 independent repetitions.
The revised clock/frame/send changes pass another complete three-pass invocation: 726/726 in each pass,
130.88/119.03/119.64 seconds. With the publication and logger regressions, three complete passes succeed at
729/729 (176.14/172.60/120.85 seconds); after the explicit Life shutdown, all 729 pass again in 122.10 seconds.
The selected browser graph also passes 20 consecutive local invocations (13 cases per invocation).

The first manual workflow, [43404654087](https://github.com/ambroise-leclerc/WebFront/actions/runs/43404654087), passes
the Linux/macOS three-pass suites, GCC/Clang CEF and both sanitizers on the diagnostics revision. Windows reaches its
25-minute job limit after a 21-minute cold MSVC analysis build. A subsequent qualification reaches its 60-minute
runner limit during stress; the explicit manual three-pass plus 20-browser-graph job now has 90 minutes.
Individual case deadlines remain unchanged.

The next workflow, [43411600509](https://github.com/ambroise-leclerc/WebFront/actions/runs/43411600509), exposes a
runner-title regression on all engines and the deferred-send registration race under TSan. Both are corrected;
that failed revision is retained as evidence and cannot qualify the final changes.

The isolated Linux closure investigation
[43425563151](https://github.com/ambroise-leclerc/WebFront/actions/runs/43425563151) passes its initial 22-case fixture
graph, then 1,000 consecutive invocations **of each** of `WebKitGTKLifecycle-navigation-allow-list` and
`JourneyEmbeddedRun`, with `--repeat until-fail:1000` on one shared Xvfb display (3353.92 seconds). It completes before
the job limit. The probe retains native stage markers, a 45-second Life phase watchdog, HTTP diagnostics and explicit
Life shutdown; the latter is also in this branch. Artifact `native-542-reproduction` retains the XML reports and
preparation transcript. This does not reproduce or identify the historical internal stall, and does not justify
describing the session-bus warning as its cause.

The main campaign [43430071781](https://github.com/ambroise-leclerc/WebFront/actions/runs/43430071781) passes GCC and
Clang CEF, all five portability configurations, ASan/UBSan (including differential gzip), TSan without the former
GLib suppression and 100 interface-loop repetitions, and GCC 13 ARM64's normal and ASan/UBSan fast tiers.
Linux and macOS pass all three complete desktop graphs. All three native engines pass all 20 selected browser
graphs, including the developer report readers on every pass. The Windows complete graph stops after pass 1 because
`WebView2Lifecycle-native-early-close` reports `STA COM initialization was not released` (job 131901058296); its other
192 cases pass. That is the independently tracked #564, explicitly outside this investigation. The workflow remains
red; its successful targeted stress step does not erase or retry that failure. All three desktop artifacts retain
the 20 stress transcripts; Windows and macOS also retain all 20 XML reports (13 cases per graph). The container's
`${{ github.workspace }}` differs from its working directory: Linux's XML reports were written outside the uploaded
build directory. Use `$PWD` for the fast/stress reports, matching the retained transcripts and complete-suite reports.
Linux's 20 successful 14-case graphs remain evidenced by both the job log and `LastTest-stress-*.log`.

Optional mddlog [43432296860](https://github.com/ambroise-leclerc/WebFront/actions/runs/43432296860) passes all 603
source-integration cases and all 603 installed-integration cases, plus both sanitizer jobs and their five randomized
transport/bridge repetitions. This follows the failed outdated-payload assertion in run 43430076833; its replacement
verifies redaction rather than restoring the removed dump.

After the explicit Life shutdown, [43433160286](https://github.com/ambroise-leclerc/WebFront/actions/runs/43433160286)
at `10672ead` passes both GCC/Clang CEF jobs and both sanitizer jobs. The final documentation/archive-path update
changes no runtime behavior; the `$PWD` report command is also exercised locally with `TestPerformanceProfile`.
