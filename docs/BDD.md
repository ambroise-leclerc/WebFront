# Testing with `webfront::bdd`

`webfront::bdd` is the behaviour-driven test framework that WebFront's own C++ tests use (#647). It is a public interface:
a project that consumes WebFront can write its scenarios with it and have CTest discover them, in place of Catch2. It is
header-only and includes only the standard library, with a CMake function that registers the scenarios.

| What | Name |
| --- | --- |
| Headers | `<webfront/testing/Bdd.hpp>` (everything), or `<webfront/testing/bdd/Core.hpp>` and the [optional parts](#headers-and-compilation-time) |
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
wait for an asynchronous state. The headers document each of them (Doxygen).

## Headers and compilation time

`Bdd.hpp` includes the whole framework. Every test file pays for what it includes, so a project with many test files can
include the light core and only the parts each file uses; the assertions are the same:

| Header | Adds | Standard headers it brings |
| --- | --- | --- |
| `bdd/Core.hpp` | `Scenario`, the sections, `require`/`check`, `requireEqual`/`checkEqual`, `requireThrows`/`checkThrows`, `requireNothrow`/`checkNothrow`, `info`, `fail`, `skip`, `warn`, `succeed`, `Listener`, `Printer` | light ones only: no `<chrono>`, `<thread>` or `<fstream>` |
| `bdd/Matchers.hpp` | `contains`, `endsWith`, `equals`, `!matcher`, `require(text, matcher)` | none beyond the core |
| `bdd/Approx.hpp` | `approx`, `Approx` | `<limits>`, `<stdexcept>` |
| `bdd/Eventually.hpp` | `requireEventually`/`checkEventually`, `requireReady`/`checkReady`, `waitUntil`, `waitReady`, `WEBFRONT_TEST_TIMEOUT_FACTOR` | `<chrono>`, `<thread>` |
| `bdd/Runner.hpp` | `run()`: the options, filters, order, shards, summary and JUnit report | `<ctime>` |

Only the file that defines `main()` needs `bdd/Runner.hpp`:

```cpp
#include <webfront/testing/bdd/Runner.hpp>

int main(int argc, char** argv) { return webfront::bdd::run(argc, argv); }
```

With GCC 16 at `-O2`, a file of one scenario compiles in about 1.2 s with `bdd/Core.hpp` and 2.0 s with `Bdd.hpp`, half of
which is `<chrono>` and `<thread>` (about 50,000 preprocessed lines instead of 100,000). A test file that includes
heavier headers of its own gains less. The `bdd_header_checks` target compiles each part alone and fails if the core
brings `<chrono>`, `<thread>` or `<fstream>` (libstdc++ and libc++).

## Comparing values

`check(a == b)` receives a `bool`: its failure shows the source line and nothing of `a` and `b`. `checkEqual(a, b)` and
`requireEqual(a, b)` report both operands:

```text
tests/Meter.cpp:42: FAILED
    checkEqual(reading.channels(), expected);
  left:  {1, 2, 3}
  right: {1, 2, 4}
```

They write strings, characters, booleans, numbers, enumerations (as their value) and ranges of these (`{a, b, c}`); a type
they cannot write appears as `{?}`, and the comparison still works. Two integers are compared by value whatever their
signedness, so `checkEqual(items.size(), 3)` neither warns nor equates `-1` with `4294967295u`, and two strings by their
characters (two `const char*` are not compared as pointers; a null one equals only another null one and is written
`nullptr`); any other pair uses `left == right`.

To write a project type, in these reports and in `info()`, `fail()` and composed section names, specialize
`webfront::bdd::Printer`:

```cpp
template <>
struct webfront::bdd::Printer<Opcode> {
    static void append(std::string& text, Opcode opcode) { text += name(opcode); }
};
```

A specialization takes precedence over the built-in forms (an enumeration can be written by name rather than as a number).

## Floating-point values

`approx(expected)` (`bdd/Approx.hpp`) compares a measured value with an expected one within a tolerance, with the semantics
of Catch2's `Approx`:

```cpp
check(voltage == approx(5.0));                       // relative epsilon, about 1.2e-5 by default
check(current == approx(0.0).margin(1e-9));          // absolute margin, 0 by default
requireEqual(frequency, approx(50.0).epsilon(1e-3)); // the report shows both operands
```

A value is accepted within either tolerance: `|value - expected| <= margin`, or `|value - expected| <= epsilon * |expected|`.
An infinity equals only itself whatever the tolerances, so a computation that overflowed is not accepted as
`approx(DBL_MAX)`. NaN is never equal. The default epsilon is `Approx::defaultEpsilon`, a hundred times the
epsilon of a `float` (about 1.19e-5), as in Catch2. A negative or NaN tolerance throws `std::invalid_argument`.
`approx` compares the numbers a `double` holds exactly: `float`, `double` and the integers of at most 53 bits. A 64-bit
integer or a `long double` wider than a `double` is refused at compile time, since beyond 2^53 a different value would
compare equal even with no tolerance: convert it explicitly with `static_cast<double>`, or compare an integer with
`checkEqual()`.
The comparison returns an object that converts to `bool` and explains a failed `check()` or `require()`:

```text
  5.5 == approx(5) (margin 0.25, epsilon 1.19209e-05): the difference is 0.5
```

More generally, `check()` and `require()` add the text of `condition.explain()` to the failure of any condition whose type
has a `std::string explain() const`.

## Listeners

```cpp
const webfront::bdd::Listener recording{
    [] { session.emplace(); },  // before the first scenario
    nullptr,                    // before each scenario
    [] { session->save(); }};   // after the last scenario
```

`Listener(runStarting, scenarioStarting, runEnding = nullptr)` takes functions without captures, or null. `runEnding`
runs once the selected scenarios have run, whatever their outcome, before the summary and the JUnit report; it runs only
when `runStarting` did, so a run that selects nothing (or only lists the scenarios) calls neither. An exception it throws
is reported and makes `run()` return 1; the JUnit report then has one more test case, `Run ending`, whose `<error>`
holds the message of each exception. A `scenarioStarting` that throws fails its scenario, which does not run, and the
run goes on, so `runEnding` is still called. A `runStarting` that throws ends the preparation of the run: the following
`runStarting` are not called and no scenario runs, but every `runEnding` is, so it must accept a run whose preparation
did not complete; `run()` returns 1 and the JUnit report has a test case `Run starting` whose `<error>` holds the
message.

`webfront_bdd_discover_tests(<target> ...)`:

| Option | Effect |
| --- | --- |
| `TEST_PREFIX <text>` | Put before `Scenario: ` in the name of each case; refused with `SHARDS` |
| `PROPERTIES <name> <value>...` | Properties of every case (`TIMEOUT`, `LABELS`, `ENVIRONMENT`...); a `SKIP_RETURN_CODE` of yours replaces the default 4 |
| `SHARDS <n>` | Register `<n>` cases, each running a share of the scenarios in one process, instead of one case per scenario |
| `SHARD_NAME <name>` | With `SHARDS` only (refused otherwise): name of the shards, `<name>-<i>` (default `<target>Shards`) |
| `JUNIT_PREFIX <path>` | With `SHARDS` only (refused otherwise): JUnit report of each shard, `<path>-<i>.xml` (default `<binary dir>/<target>-results-<i>.xml`) |

A value of `PROPERTIES` is kept as one argument whatever it holds (a `;`, a trailing backslash, `]==]`, even the name of an option such as `SHARDS`), so pass such a value as one quoted argument; CMake then reads a `;` in it as a list separator, as `set_tests_properties` does (`ENVIRONMENT "A=1;B=2"` sets two variables). `PROPERTIES` takes pairs of a name and a value.

Call it once per target without `SHARDS`: a second such call on the same target is refused (it would overwrite the first one's files). A call with `SHARDS` registers its cases at once and can accompany it.

A change of `PROPERTIES` or `TEST_PREFIX` links the executable again (Makefile and Ninja generators), so that building the target alone registers the new values; a complete build does it with any generator, unless the executable is `EXCLUDE_FROM_ALL` (set before the call), which discovery never brings back.

After each build the executable lists its scenarios (`--list`) and a post-build step writes one CTest case per scenario,
`Scenario: <title>`, whatever brackets or semicolons the title holds. Each case runs `<target> --exact <title>`; a
scenario that skips itself exits with 4, which the cases treat as skipped. When the target has a `CROSSCOMPILING_EMULATOR`
(or `CMAKE_CROSSCOMPILING_EMULATOR` is set), it lists the scenarios and runs every case.

The executable's own options are `--list`, `--exact`, `--order`, `--rng-seed`, `--shard-count`, `--shard-index`, `--junit`
and the name filters; run it with `--help`.

## Options and what stays internal

- `WEBFRONT_TEST_TIMEOUT_FACTOR` is an environment variable read by the framework: a decimal such as `2` or `2.5` that
  multiplies the delays of `requireEventually` and its siblings, for a slow job (sanitizers, emulation). It is public.
  CMake does not read it for you: give your cases a `TIMEOUT` that agrees, or pass the variable through `ENVIRONMENT`.
- `WebFront_test_hardening`, `WebFront_warnings`, `WebFront_options`, `WEBFRONT_TEST_UNIT_SHARDS` and
  `TimeoutFactor.cmake` belong to WebFront's own tests. They are not part of the interface and are not installed.

## Compatibility

The headers (`Bdd.hpp` and the parts under `bdd/`), the namespace `webfront::bdd` and its names, the semantics above, the `WebFront::Bdd` target and
`webfront_bdd_discover_tests` with its options follow WebFront's semantic versions: an incompatible change waits for a
major version and is listed under `Breaking` in the [changelog](CHANGELOG.md); the framework's additions appear under
`Added`. The installed package accepts a WebFront of the same major version (`SameMajorVersion`). What the headers
document is the interface; the contents of `webfront::bdd::impl` is not.
`BddSelfTest`, `BddDiscovery` and `BddConsumer` (a project that finds the installed package) pin it.

The headers compile as C++23 and, in a C++26 translation unit, as C++26; an executable must not mix the two standards.
