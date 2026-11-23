# Testing with `webfront::bdd`

`webfront::bdd` is the behaviour-driven test framework that WebFront's own C++ tests use (#647). It is a public interface:
a project that consumes WebFront can write its scenarios with it and have CTest discover them, in place of Catch2. It is a
single header that includes only the standard library, with a CMake function that registers the scenarios.

| What | Name |
| --- | --- |
| Header | `<webfront/testing/Bdd.hpp>` |
| Namespace | `webfront::bdd` |
| CMake target | `WebFront::Bdd` (`INTERFACE`: the header, C++23 and threads; it needs neither WebFront nor stdnet) |
| CMake function | `webfront_bdd_discover_tests()`, the counterpart of `catch_discover_tests` |
| Package | `WebFrontBdd` (`find_package(WebFrontBdd)`) |

## Getting it

With CPM, FetchContent or `add_subdirectory`, the target and the function exist once WebFront is added; nothing else is
built or installed from a project that embeds WebFront:

```cmake
CPMAddPackage("gh:ambroise-leclerc/WebFront@<version>")
```

From an installed WebFront (`cmake --install <build> --prefix <prefix> --component WebFrontBdd` of the top-level project), use the package; it
installs the header, `WebFront::Bdd` and the module that defines `webfront_bdd_discover_tests()`:

```cmake
find_package(WebFrontBdd REQUIRED)    # with -DCMAKE_PREFIX_PATH=<prefix>
```

## Writing and registering scenarios

```cpp
#include <webfront/testing/Bdd.hpp>

using namespace webfront::bdd;

const Scenario addition{"Addition", [] {
    int sum = 0;
    given("two numbers", [&] {
        when("they are added", [&] {
            sum = 2 + 3;
            then("the sum is five", [&] { require(sum == 5); });
        });
    });
}};

int main(int argc, char** argv) { return run(argc, argv); }
```

```cmake
add_executable(my_tests main.cpp)
target_link_libraries(my_tests PRIVATE WebFront::Bdd)
webfront_bdd_discover_tests(my_tests TEST_PREFIX "My." PROPERTIES TIMEOUT 30 LABELS fast)
```

As in Catch2, a scenario runs again from its start until each leaf section has run once. `require` ends the scenario at the
first failure, `check` records it and goes on, and `requireEventually`, `checkEventually`, `requireReady` and `checkReady`
wait for an asynchronous state. The header documents each of them (Doxygen).

`webfront_bdd_discover_tests(<target> ...)`:

| Option | Effect |
| --- | --- |
| `TEST_PREFIX <text>` | Put before `Scenario: ` in the name of each case; refused with `SHARDS` |
| `PROPERTIES <name> <value>...` | Properties of every case (`TIMEOUT`, `LABELS`, `ENVIRONMENT`...); a `SKIP_RETURN_CODE` of yours replaces the default 4 |
| `SHARDS <n>` | Register `<n>` cases, each running a share of the scenarios in one process, instead of one case per scenario |
| `SHARD_NAME <name>` | With `SHARDS` only (refused otherwise): name of the shards, `<name>-<i>` (default `<target>Shards`) |
| `JUNIT_PREFIX <path>` | With `SHARDS` only (refused otherwise): JUnit report of each shard, `<path>-<i>.xml` (default `<binary dir>/<target>-results-<i>.xml`) |

A value of `PROPERTIES` that holds a `;` is not supported: CMake splits it into several values before the function sees it (`ENVIRONMENT "A=1;B=2"` becomes two), so give such a variable to `ctest` through its own environment instead.

Call it once per target without `SHARDS`: a second such call on the same target is refused (it would overwrite the first one's files). A call with `SHARDS` registers its cases at once and can accompany it. A value of `PROPERTIES` must not contain `]==]`.

After each build the executable lists its scenarios (`--list`) and a post-build step writes one CTest case per scenario,
`Scenario: <title>`, whatever brackets or semicolons the title holds. Each case runs `<target> --exact <title>`; a
scenario that skips itself exits with 4, which the cases treat as skipped.

The executable's own options are `--list`, `--exact`, `--order`, `--rng-seed`, `--shard-count`, `--shard-index`, `--junit`
and the name filters; run it with `--help`.

## Options and what stays internal

- `WEBFRONT_TEST_TIMEOUT_FACTOR` is an environment variable read by the framework: a decimal such as `2` or `2.5` that
  multiplies the delays of `requireEventually` and its siblings, for a slow job (sanitizers, emulation). It is public.
  CMake does not read it for you: give your cases a `TIMEOUT` that agrees, or pass the variable through `ENVIRONMENT`.
- `WebFront_test_hardening`, `WebFront_warnings`, `WebFront_options`, `WEBFRONT_TEST_UNIT_SHARDS` and
  `TimeoutFactor.cmake` belong to WebFront's own tests. They are not part of the interface and are not installed.

## Compatibility

The header, the namespace `webfront::bdd` and its names, the semantics above, the `WebFront::Bdd` target and
`webfront_bdd_discover_tests` with its options follow WebFront's semantic versions: an incompatible change waits for a
major version and is listed under `Breaking` in the [changelog](CHANGELOG.md); the framework's additions appear under
`Added`. The installed package accepts a WebFront of the same major version (`SameMajorVersion`). What the header
documents is the interface; the contents of `webfront::bdd::impl` is not.
`BddSelfTest`, `BddDiscovery` and `BddConsumer` (a project that finds the installed package) pin it.

The header compiles as C++23 and, in a C++26 translation unit, as C++26; an executable must not mix the two standards.
