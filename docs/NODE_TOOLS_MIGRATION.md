# Node.js tools: inventory and destination

Transition note of the epic #303 (issues #315 and #318). At the close of the epic no WebFront path depends on Node.js,
npm or npx; until then the Node.js tools of `js/developer/` remain, and this list gives each one's destination. #318
deletes a tool only after the destination below is in place and proven; nothing is kept as a fallback.

The environment builds itself since #315: `webfront.project.json` and `webfront-packages.lock.json` make the repository a
`wf` project, and `js/developer/scripts/` holds the scripts that the pinned bootstrap environment runs with
`webfront-wf` (see `js/developer/tools/PUBLISHING.md`). Their outputs are byte for byte those of the Node.js build, which
the CTest cases `SelfBuild*` and the `wf-package` job of `DeveloperDistribution.yml` require during the transition.

| Entry | Role | Destination | State |
| --- | --- | --- | --- |
| `tools/build.mjs` | environment, starters, header, bootstrap bundles | `scripts/build.mjs` (`wf run build`) | migrated (#315); the Node.js file is deleted by #318 |
| `tools/archive.mjs` | reproducible ustar | unchanged: no Node.js API, imported by the wf scripts | kept |
| `tools/distribution.mjs` (new) | versions, URLs, pinned header, published files | shared by the wf scripts and the Node.js tools | kept |
| `tools/asset-header.mjs` (new) | header of an embedded asset | shared by `scripts/build.mjs` and `vendor/embed.mjs` | kept |
| `tools/bundle-inputs.json` (new) | inputs of the bundle checksums | read by both builds | kept |
| `tools/site.mjs` | staging of the Pages site | `scripts/site.mjs` (`wf run site`) | migrated (#315); the Node.js file is deleted by #318 |
| `tools/pin-check.mjs` | pinned header against the published archive | `scripts/pin.mjs` (`wf run pin`, `permissions.fetch`) | migrated (#315); deleted by #318 |
| `tools/pages.mjs` | Pages source check and build request | `tools/pages.sh` (`gh`) | migrated (#315); deleted by #318 |
| `tools/archive-release.mjs` | prerelease archive | `tools/archive-release.sh` (`gh`, `jq`) | migrated (#315); deleted by #318 |
| `tools/release-check.mjs` | dedicated tag check | step of `wf-package` (shell) | migrated (#315); deleted by #318 |
| `tools/fixtures.mjs`, `registry-fixture-node.mjs` | offline registry and test pages from `npm ci` | tarballs pinned by name, version and SHA-512, downloaded by CMake at configure, and a wf script for the synthetic packages (#319) | #318, #319 |
| `tools/registry-fixture.mjs` | pure part of the registry fixture | wf script of #319 | #318, #319 |
| `tools/check.mjs`, `manifest-check.mjs`, `restore-check.mjs`, `scripts-check.mjs`, `development-check.mjs`, `testing-check.mjs` | pure JavaScript tests (`node --test`) | Jasmine specs run by `wf test`, in the `web-integration` suite | #318 |
| `tools/distribution-check.mjs` | starters, staging of the site | starters: Jasmine specs (#318); staging: `SelfBuildSite*` (#315) | partly migrated |
| `tools/memory-bridge.mjs` | in-memory bridge of the tests | moves with the specs | #318 |
| `vendor/embed.mjs`, `js/core/EmbedTextAsset.cmake` | core header generator | `scripts/embed-core.mjs` (`wf run embed-core`) produces the same bytes (`SelfBuildCoreHeaders`); #318 chooses between it and a pure CMake generator, in ADR-016 | available (#315); switch and deletion by #318 |
| `vendor/embed.test.mjs` | generator tests | CMake tests for processes and files, the browser for gzip | #318 |
| `vendor/package.json`, `package-lock.json` | `npm ci` of the maintainers | `webfront.project.json` and its lock; the fixture packages by #319 | #318, #319 |
| README and `DEVELOPER_ENVIRONMENT.md`: `node -p` extracting the integrity, `npm ci`; `CheckNewcomerDocs.cmake` pins that text | local review of an unpublished archive | the commands read `provision.json` without Node.js | #317, #318 |
| `BuildAndTest.yml`: `setup-node`, `npm ci`, `node build.mjs`, `node fixtures.mjs` | fixtures of the desktop and CEF jobs | the environment built by wf, the registry of #319 | #318 |
| `DeveloperDistribution.yml`: `package` job | Node.js build and tests | removed with the comparison | #318 |
| `WebFrontAssets.cmake`: `DeveloperJavaScript`, `EmbeddedAssetGenerator`, asset freshness | Node.js tests | as above; `WEBFRONT_REQUIRE_NODE=OFF` does not register them | #318 |

`starters/`, `distribution.json`, `PUBLISHING.md` and `RELEASE_NOTES.md` are data and documents, kept.

## What needs no Node.js already

- Configuring and building `webfront-wf` (`-DWEBFRONT_REQUIRE_NODE=OFF`), as the `wf-package` and `publish` jobs do after
  removing `node`, `nodejs`, `npm`, `npx` and `corepack` from the runner.
- Restoring the locked build dependencies (`wf restore`), building the environment, the starters and the pinned header, the
  bootstrap bundles and the core headers they embed, twice, with identical bytes.
- Staging a version in the Pages site (immutable versions), checking the pin against the published archive, checking the
  Pages source, requesting its build and archiving a prerelease.

## What still needs it

Tests of the pure JavaScript, the asset freshness checks and the generator of the core headers, the fixtures of the
browser tests (the registry and the test pages) and the desktop and CEF jobs that prepare them. The closing proof of #315,
from an empty cache and with the pinned SHA-512 of the first environment that builds itself, is the publication of that
environment: the Node.js tools are deleted only after it.
