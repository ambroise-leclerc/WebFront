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
