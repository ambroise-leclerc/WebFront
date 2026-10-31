# Pull-request CI time (#435)

Pull-request runs of `BuildAndTest.yml` took about 6 minutes, or 20 when the pull request touched a header that every heavy
unit includes (`WebFront.hpp`: #400, #401, #428). The cause is compile time: about 26 translation units instantiate a full
application and cost 28 to 113 s each on MSVC, and a changed header misses the compiler cache for all of them.

This page keeps what was measured and what was decided. The measurements come from GitHub-hosted runners, with a throwaway
workflow (in the history of pull request #436) that timed clean builds of the `tests` target with an empty compiler cache
(`cold`), a complete one (`warm`) and a complete one but for the files a pull request changes (`header`, `heavy-test`,
`light-test`). Hosted Windows runners vary by up to 1.5x between identical jobs, so Windows figures are the median of three
runs; Linux runs are steadier.

## Decisions

| Change | Effect |
|---|---|
| `--parallel 4` on the desktop builds | Linux `tests` 280 -> 170 s cold, `all` 520 -> 255-375 s; Windows `tests` 820 -> 640 s, `all` 1240 -> 1080 s. Peak memory stays under 1 GB. |
| The 13 heavy unit tests compile four to a file (`WEBFRONT_TEST_UNITY_BATCH=4` on the desktop jobs) | Windows cold 640 -> 355 s, `WebFront.hpp` changed 298 -> 111 s; Linux 202 -> 143 s and 88 -> 45 s. Less memory. |
| MSVC `/analyze` and the examples only in the master and weekly tier | `/analyze` is about 22 % of a Windows build (640 -> 497 s cold); the examples about 190 s of compiler time. |
| The reproducibility builds only when a pull request changes their inputs | About 45 s of worker time per job (`SelfBuildRepeat`, `SelfBuildRefresh`). |
| `JourneyEmbeddedBuild` and `LifeEmbeddedBuild` do not hold `webfront_port_9002` | They only compile; the lock had serialised 175 s (Linux) and 203 s (Windows) of tests. |
| `RunWithDeadline.sh` returns when its command does | Six Linux `ConsumerJasmine-*` cases waited 30 s each for nothing. |
| `DeveloperBrowserIntegration` in five parts | The dependency chain 186 -> 130 s on Linux; see below. |

On a pull request that touches a core header (this one, after merging `develop`), the build step went from 328 to 85 s on Linux
and from 876 to 213 s on Windows, and the Windows job from about 20 to 7.7 minutes.

## Measured and not adopted

- **Precompiled headers.** sccache cannot cache an MSVC compilation that uses one, so a complete cache still recompiles every
  unit (warm 32 -> 231 s on Windows) and the build needs 1.7 GB. On GCC it was slower than no precompiled header.
- **Grouping every test file** does not compile: the light units share helper names. Only the heavy units are grouped, and the
  helpers they share were renamed. Groups of 4 beat groups of 6 and 12 in every scenario on both systems: a change to one
  heavy test recompiles at most three others, and four groups still spread over four jobs.
- **Sharing instantiations between executables** (`extern template`). `life_journey` and `life_embedded` compile the same file
  against different generated headers, and the developer application is instantiated with a different frontend or filesystem
  in each executable. Identical function instantiations that occur in more than one unit of `tests` account for at most 9 %
  of its compile time (Clang `-ftime-trace`, an upper bound).
- **Merging the small executables** (`jasmine_consumer`, `frontend_lifecycle`, `webview2_*`, 66 to 69 s each on MSVC): each
  instantiates its own frontend type, and separate processes are part of what several cases prove.

Where one compile goes (the floor of a heavy unit): `HelloWorld.cpp` costs 11 s with GCC (Debug, coverage: 21 % parsing,
23 % instantiation, 49 % optimisation and code generation), 22 s with Clang, 61 s with MSVC, where `WebFront.hpp` alone takes 8 s
to parse and the front end runs a second time for `/analyze`.

## Run time

The suite took 256 s (Linux) and 271 s (Windows). Its chain of dependent cases is `SelfBuildSession` -> `DeveloperFixtures`
-> `DeveloperBrowserIntegration` -> `LifeEmbeddedBuild` -> `LifeEmbeddedIntegration`: the first builds the environment under test
from the current sources and the later ones run it, so the order cannot be shortened.

`SelfBuildSession` (59 s on a loaded Linux runner, 44 s on a quiet machine) is five `wf` commands in one browser session:
install 0.4 s, build the environment 8.5 s, its 121 JavaScript specs 16.3 s, a second build to prove the bytes equal 11.7 s,
and the refusal of a write outside the permissions 6.4 s. It is a product build and the test suite of the environment, run
by the product itself in WASM, so it cannot be a 3 s test.

`DeveloperBrowserIntegration` was one script in one browser over one project (59 to 113 s). It is five parts
(`--part=app|scripts|typescript|jasmine|terminal`), each from its own project on a port the system picks, so none holds the port
lock. On a quiet Mac each part takes 3 to 6 s. On a four-vCPU runner the chain is shorter and the suite is not faster:
with four workers it is bound by its worker time divided by four (735 s / 4 = 184 s on Linux against a wall time of 195 s),
and the parts add worker time (each repeats the bootstrap, `wf create` and `wf init`, and five browsers contend for four cores).
Shortening the suite further means removing worker time; the largest cases on Linux are `AutomationMcpWindow` (62 s),
`SelfBuildSession` (59 s), `SelfBuildRefresh` (34 s) and `JourneyBuildSession` (31 s).

`FrontendConsumerConfiguration` and `AddSubdirectoryNativeConsumer` (about 65 s each on Windows) configure and build separate
projects on purpose, each proving another option set from a clean state; they are off the chain.
