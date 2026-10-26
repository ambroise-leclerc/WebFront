# Test scheduling and CI diagnostics

The CI uses `ctest --parallel 4`, independently of `cmake --build --parallel 2`.
Desktop, sanitizer and CEF jobs retain JUnit durations and `LastTest.log` even on
failure. Manual dispatch with `repeat_desktop_tests=true` repeats each desktop
suite three times and also runs the extended CEF and sanitizer checks. The CI
uses `test/cmake/RunTestSuite.cmake`: each pass starts a new CTest invocation,
so fixtures reset before their stateful dependents run. Repeating individual
cases with `--repeat` cannot validate the create/init/install/restore journey.
Each pass retains its own `test-results-<pass>.xml` report and the first failed
suite stops the runner with a nonzero status.
Numbered reports from earlier invocations are removed before the first pass,
including when a rerun requests fewer passes or fails early.

The developer integration prints phase markers before compilation, previews,
scripts, type checks, Jasmine runs and terminal checks. Failed macOS desktop
jobs also retain `build/diagnostics/` (WebKit system log and available crash
reports), so a terminated content process can be investigated beyond its final
frontend error. Missing crash reports do not change the test's failure status.
The developer specs print each started spec through the driver's bridge, retaining
the last name if the page stalls. Self-build drivers have a native watchdog that
expires before CTest's timeout, including its forced-close grace period.
The progress callback is optional: running the specs in an ordinary developer
application or losing the diagnostic bridge does not change their results.

## Resource audit

| Tests | Scheduling constraint |
| --- | --- |
| Catch2 scenarios | Separate processes; mutable globals and current directories are process-local. Filesystem fixtures atomically claim temporary directories; the one relative `test.tmp` is used by one scenario only. Real socket tests use ephemeral ports, including developer bootstrap without a project. |
| Asset checks | Read sources and compare full headers in memory; no shared temporary gzip/header output. |
| JavaScript and attribution regressions | Own temporary directories/repositories. |
| Frontend selection | Its own `frontend-tests` directory; multi-config tests have a separate working directory. Each configure starts with a fresh fixture cache. |
| CEF option scope | Its own `cef-testing-state` directory; each state still configures separately. |
| Consumer | Separate `consumer-build` tree; no browser is opened. |
| Journey | One project directory (`<build>/jp`) chained by CTest fixtures; the browser lock; the nested build is `RUN_SERIAL`. |
| Browser integration and lifecycle | Keep `RESOURCE_LOCK webfront_port_9002`, including the shared CEF profile and generated browser resources. |
| Life embedded build | Keep `FIXTURES_REQUIRED DeveloperGenerated`, then the build/run fixture dependencies. `RUN_SERIAL TRUE` excludes every other test while it runs, protecting running executables from regeneration/relinking. `PROCESSORS 2` records the nested build's two workers. |

The browser setup must finish generating headers before the Life embedded build
starts. Browser tests remain mutually exclusive. No test runs alongside the nested build. The consumer has a different
build tree; ordinary independent tests can overlap when the embedded build
is not running.

Linux coverage processes still write GCC's normal `.gcda` files on the runner's
local filesystem, using libgcov's file locking. Do not copy this parallel coverage
command to a filesystem without locking support without isolating and merging
the profiles. Sanitizer test processes retain their existing runtime settings.

## Reproduction

After configuring and building dependencies, run offline from the checkout:

```sh
ctest --test-dir build --output-on-failure --parallel 1 --output-junit "$PWD/build/serial.xml"
ctest --test-dir build --output-on-failure --parallel 4 --output-junit "$PWD/build/parallel.xml"
cmake -DBUILD_DIR="$PWD/build" -DREPEAT=3 -P test/cmake/RunTestSuite.cmake
ctest --test-dir build-cef -L web-integration --output-on-failure --parallel 4
```

Compare both the total real elapsed time printed by CTest and the individual
JUnit `testcase` durations; summing parallel test times overstates wall time.
For browser/build comparisons, record cold and warm compilation separately.
To repeat every case on the three hosted desktop platforms while also running the
extended CEF/sanitizer checks:

```sh
gh workflow run BuildAndTest.yml --ref <pr-branch> -f repeat_desktop_tests=true
```

Use the ordinary PR run (one repetition) for before/after performance comparisons;
the three-repeat run is a stability check and its wall time includes repetitions.
Every remaining test over 20 s should be reviewed against the costs above rather
than disabled or given weaker assertions.
