# Node.js tools: where each one went

Record of the transition of the epic #303 (issues #315 and #318). It is a migration note, not a guide: no WebFront path
depends on Node.js, npm or npx any more, and nothing was kept as a fallback. The `NoNodeDependency` CTest case audits every
tracked file; this note and the files of the guard are among its listed exceptions (`test/cmake/NodeGuardExceptions.cmake`).

## Tools

| Former entry | Role | Destination |
| --- | --- | --- |
| `js/developer/tools/build.mjs` | environment, starters, header, bootstrap bundles | `js/developer/scripts/build.mjs` (`wf run build`), since #315; the Node.js file was deleted by #318 |
| `tools/site.mjs` | staging of the Pages site | `scripts/site.mjs` (`wf run site`); its test, once on a temporary directory, now runs on an in-memory project in `distribution-specs.mjs` |
| `tools/pin-check.mjs` | pinned header against the published archive | `scripts/pin.mjs` (`wf run pin`) |
| `tools/pages.mjs`, `tools/archive-release.mjs`, `tools/release-check.mjs` | Pages source check, prerelease archive, tag check | `tools/pages.sh`, `tools/archive-release.sh` (`gh`, `jq`) and a step of `wf-package` |
| `tools/fixtures.mjs`, `tools/registry-fixture-node.mjs` | offline registry and test pages from `npm ci` | `cmake/WebFrontRegistry.cmake` downloads the pinned archives at configure; `wf run test-registry` builds the registry; the CTest fixture `DeveloperFixtures` assembles the rest |
| `tools/registry-fixture.mjs` | pure part of the registry fixture | `test/web/registry/fixture.mjs` (only browser APIs) |
| `tools/memory-bridge.mjs` | in-memory bridge of the tests | `test/developer/memory-bridge.mjs` |
| `tools/asset-header.mjs`, `vendor/embed.mjs`, `js/core/EmbedTextAsset.cmake`, `scripts/embed-core.mjs` | rendering of the core headers | `js/core/EmbedAsset.cmake`, pure CMake (ADR-016, v1.3) |
| `vendor/package.json`, `package-lock.json` | `npm ci` of the maintainers | `webfront.project.json` and its lock; the registry packages are listed in `test/web/registry-packages.json` |
| `tools/archive.mjs`, `tools/distribution.mjs`, `tools/bundle-inputs.json` | ustar writer, rules of the distribution, inputs of the bundle checksums | unchanged: they use no host API and are read by the wf scripts |
| `BuildAndTest.yml`: Node.js setup, package installation, `build.mjs` and `fixtures.mjs` steps | fixtures of the desktop and CEF jobs | none: configure downloads, the tests build; every job removes the JavaScript runtimes from the runner first |
| `DeveloperDistribution.yml`: `package` job and its comparison | Node.js build | deleted; `wf-package` also runs `wf test` |
| README and guides: extraction of the integrity with a Node.js one-liner | review of a local archive | `sed` on `provision.json` (`CheckNewcomerDocs.cmake` pins the text) |

## Tests

86 tests of the Node.js runner, none lost:

| Former file (tests) | Destination |
| --- | --- |
| `check.mjs` (14) | `test/developer/specs/environment-specs.mjs` (12 specs); the checksums of the bundle inputs became `BundleInputsFresh` and `BundleInputsRejectsChange` (CMake), the command names reserved by the terminal and by `webfront::Commands` became `CommandNamesAgree` (CMake) |
| `manifest-check.mjs` (16) | `manifest-specs.mjs` |
| `restore-check.mjs` (12) | `restore-specs.mjs` |
| `scripts-check.mjs` (21) | `scripts-specs.mjs`; the Worker module is evaluated afresh for each spec from the project's own source |
| `development-check.mjs` (7) | `development-specs.mjs`; the module is imported directly instead of bundled by a native compiler |
| `testing-check.mjs` (11) | `testing-specs.mjs`, compiling with the locked esbuild-wasm |
| `distribution-check.mjs` (5) | `distribution-specs.mjs` |
| `embed.test.mjs` (5) | `EmbeddedAssetGenerator` (`CheckEmbedAsset.cmake`: format, padding, exit status, path with spaces, symbolic link), the Catch2 scenario on the embedded assets of the core, the per-asset `*AssetFresh` cases. The gzip cases went with the gzip rendering of the core, which had no user left |

The specs run with `wf test` on the repository (`webfront.project.json`, `test.specs`); the CTest case `DeveloperSpecs` runs them
on a copy of the sources, and the `wf-package` job on the checkout with its lock.
