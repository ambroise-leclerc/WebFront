# Test tiers (#388)

The suite has three tiers, selected by CTest labels. A test carries at most one of them; a new test with no label belongs
to the standard tier.

| Tier | Label | Content | When it runs |
| --- | --- | --- | --- |
| Fast | `fast` | Catch2 unit tests and the Python tool tests; no browser, no nested build | Locally on demand, and as the first test step of every Linux/Windows/macOS desktop job |
| Standard | none, or `web-integration` | Everything but the portability tests: unit tests, real-browser integration, `wf` sessions, embedded builds | Every pull request and every push to `develop` or `master` |
| Portability | `portability` | The library consumed from another project: `AddSubdirectoryConsumer` | Weekly, and on every pull request into `master` |

```sh
ctest --test-dir build -L fast --parallel 4                # about one second
ctest --test-dir build -LE portability --parallel 4        # the standard tier
ctest --test-dir build -L portability                      # the portability tier
cmake -DBUILD_DIR=build -DLABEL_EXCLUDE=portability -P test/cmake/RunTestSuite.cmake
```

`RunTestSuite.cmake` runs every test unless `LABEL_EXCLUDE` names a label; the CI standard tier passes
`portability`. A local run before a change is declared complete (see `AGENTS.md`) runs everything.

## Following a browser test live

`DeveloperBrowserIntegration`, the `Automation*` cases and the `WfDriver*` cases run `wf` commands (`create`, `install`,
`build`, `dev`, `preview`, `embed`, ...) that can take several seconds each. CTest hides the output of a passing test, so
to watch one:

```bash
ctest --test-dir build -R DeveloperBrowserIntegration -V        # CTest prints the output as it arrives
ctest --test-dir build -R DeveloperBrowserIntegration -N -V     # prints the command to run `developer_integration` directly
```

The process prints a `Developer integration: <time>ms begin|end <step> durationMs=...` line for each phase and each
command. The window of `DeveloperBrowserIntegration` also shows, in place of its terminal, every `wf` command the specs
run with its outcome and duration, as if it had been typed. Only the `wf` subcommand is shown, never an argument, an
option value or a path. A failed command appears as `rejected`, with its exit code when it has one, whether or not the
spec expected the refusal. A wait is labelled `timeout (expected)` only for a time limit the spec announces and fires,
and `cancelled (expected)` only when the spec pressed Ctrl+C; any other time limit or cancellation appears as `timeout`
or `cancelled`. This trace comes from the specs (`test/web/developer-specs.mjs`), not from the published environment.

## Advanced checks

These run on the weekly schedule (Monday 04:23 UTC), on every pull request whose base is `master`, and on a manual
dispatch with `repeat_desktop_tests`:

- the complete desktop suite on the three desktop platforms, repeated three times with its fixtures reset on each pass,
  including the portability tests;
- the `portability` job: the consumer project configured with Ninja (GCC on Linux, MSVC on Windows, Apple clang on macOS),
  with Unix Makefiles (Clang 23) and, reported but never blocking until it has run on a hosted runner, the Visual Studio
  2026 generator. There is no compiler cache: a new user compiles everything;
- the address/undefined and thread sanitizers, the five fuzzers, and the GCC and Clang CEF browser integration.

The portability matrix does not vary the CMake version: `setup-cmake` pins the SHA-256 of each archive it installs, so
another version needs its checksum added first.

## Required checks for `master`

A pull request into `master` must pass all of: the desktop jobs, `portability` (except the experimental Visual Studio
leg), `sanitizers` and `web-integration`. GitHub only enforces this once the checks are marked required in the branch
protection rules of `master`; the workflow cannot set that itself.

The fast tier does not replace the others: the unit tests are also part of the standard run, so a pull request still
reports one complete result.
