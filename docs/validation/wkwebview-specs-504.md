# WKWebView asynchronous specs (#504)

## CI investigation — 2026-11-10

Sample: the latest 70 `BuildAndTest.yml` runs returned by `gh run list --workflow BuildAndTest.yml --limit 70`, from
2026-11-09 08:40 UTC to 2026-11-10 10:54 UTC. For each run, `/actions/runs/<id>/jobs?filter=all&per_page=100` includes
reruns rather than hiding an initial failure behind a successful retry. The macOS job appears in 68 attempts: 56 successful,
7 failed, 4 cancelled and 1 still running at collection. Pushes to `develop` account for 12 attempts (11 successful,
1 running); pull requests account for 56 (45 successful, 7 failed, 4 cancelled). The other runs do not contain this job.

The failed macOS job logs show:

| Run | Failure |
| --- | --- |
| [42587615911](https://github.com/ambroise-leclerc/WebFront/actions/runs/42587615911) | File-services: one of three specs exceeds Jasmine's 5 s deadline. |
| [42604656914](https://github.com/ambroise-leclerc/WebFront/actions/runs/42604656914) | `NewcomerDocsMatchExamples`; unrelated. |
| [42606639237](https://github.com/ambroise-leclerc/WebFront/actions/runs/42606639237) | Video: Jasmine's 5 s deadline; the same commit passes on attempt 2. |
| [42640365121](https://github.com/ambroise-leclerc/WebFront/actions/runs/42640365121) | File-services: one of three specs exceeds Jasmine's 5 s deadline. |
| [42648156273](https://github.com/ambroise-leclerc/WebFront/actions/runs/42648156273) | Self-build/compilation; unrelated. |
| [42689059284](https://github.com/ambroise-leclerc/WebFront/actions/runs/42689059284) | Video: `Expected 0 to be greater than 9` after the first `seeked`. |
| [42705081084](https://github.com/ambroise-leclerc/WebFront/actions/runs/42705081084) | Frontend selection configure checks; unrelated. |

The problem recurs across different PRs. The historical file-services reports have discarded spec names and contain only
the generic timeout: they cannot identify which spec or operation stalled. Runner contention remains a hypothesis,
not an established cause. The position failure is consistent with an initial positioning event racing the requested seek;
the log alone does not establish the engine's internal ordering.

## Changes and failure diagnostics

- A shared test-only helper gives each affected spec one 15 s budget, with Jasmine's outer deadline at 20 s. This is
  local to these specs on every engine; other specs keep their existing limits. There are no retries or skipped checks.
- Every asynchronous file-services operation names its stage, including `change`, `text()`, `arrayBuffer()`, the bridge
  call and both native captures. The generated Blob URL is revoked even on failure. The runner's application deadline
  is 60 s; the outer CTest deadline is 75 s (`TIMEOUT + 15` in `cmake/WebFrontJasmine.cmake`).
- Video waits for initial data after metadata, then accepts `seeked` only at the requested destination with `seeking`
  false. Both seek directions, playback and observed HTTP requests are still checked. Event listeners and timers are
  removed on success, media error, rejected `play()` or timeout.
- A failed video wait records its stage, `readyState`, `networkState`, `currentTime`, duration, paused/seeking flags and
  `mediaRanges`. The diagnostic bridge query has its own 1 s limit, so a stalled bridge cannot hide the original failure.
- Consumer Jasmine failure strings preserve the spec/suite name. The existing expected-failure consumer verifies it
  survives into the JSON report. Seven additional browser specs exercise missing events, a stalled `play()`, synchronous
  events, media/play errors, unrelated queued seeks, stalled native operations, late rejection and budget exhaustion.
  The review follow-up adds expected-failure consumers for Unicode and 50 oversized diagnostics: reports normalize
  isolated surrogates and shorten message prefixes on character boundaries to fit the UTF-8 JSON budget, retaining all
  recorded failures and their names. Any shortening is explicit in `truncated`; names are shortened only if they cannot
  fit even after every diagnostic is empty.

No resource lock was added: the sample does not demonstrate contention as the cause, and serialization would prevent
testing the scenarios under the parallel load that exposed the problem.

## Local validation

On macOS 27.2, Apple M2, AppleClang 21.0.0: the Release build succeeds and the complete CEF-off/WKWebView CTest
suite passes **706/706** tests with four workers (530 `fast`, 135 `web-integration`, 1 `portability`; 139.21 s wall time).
`ConsumerJasmine-async-diagnostics` executes all seven diagnostic regression specs successfully, and
`ConsumerJasmineReports` verifies the full failing spec name and assertion in the consumer report.

After the reporting review fixes, the Release build and complete CTest suite pass **709/709** tests in 108.66 s with
four workers. The new expected-failure consumers preserve five Unicode/name diagnostics (including isolated
surrogates, a 3000-character name and a leading U+FEFF) and all 50 oversized diagnostics; their JSON reports are parsed
by C++ and checked for byte size, complete names, retained prefixes and explicit shortening by `ConsumerJasmineReports`.

The first repetition attempt passed file-services 50/50 and the full browser suite 24 times, then CTest stopped the
25th browser process at 125 s before any Jasmine output. The macOS power log records `Idle Sleep` at 12:12:47 CET for
125 s, a dark wake at 12:14:52 and full wake at 12:14:54; a subsequent single browser run passes in 15.35 s. This is
a suspended-host interruption, not evidence of a media event timeout. The uninterrupted qualification is run under
`caffeinate -di`, which prevents idle system/display sleep only for the duration of CTest.

The subsequent uninterrupted run passes **50/50** executions of `ConsumerJasmine-file-services` and **50/50** executions
of `WebFrontWKWebViewIntegration` (136 Jasmine specs per execution), with no timeout or failed assertion, in 899.37 s
wall time. Both tests run under the same CTest invocation with four workers; once file-services finishes its 50 runs,
the remaining browser-suite executions run alone. The temporary power assertions end with CTest.

Use an out-of-source Release build with `WEBFRONT_ENABLE_TESTING=ON`, `WEBFRONT_EMBED_CEF=OFF`,
`WEBFRONT_EMBED_WKWEBVIEW=ON` and `WEBFRONT_WEBTEST_FRONTEND=WKWebView`.
After building, first run the complete CTest suite, including its fixtures. Then repeat only the two independent browser
processes, retaining their installed fixtures (the normal integration test still executes all browser specs):

```sh
ctest --test-dir build-504-wk --output-on-failure --parallel 4
caffeinate -di ctest --test-dir build-504-wk --output-on-failure --parallel 4 \
  -R '^(ConsumerJasmine-file-services|WebFrontWKWebViewIntegration)$' \
  --fixture-exclude-any '.*' --repeat until-fail:50
```

These local repetitions do not reproduce the hardware or contention of a hosted macOS runner. CEF/Xvfb and the
required GCC/Clang CEF workflow validation remain separate Linux checks before merging.
