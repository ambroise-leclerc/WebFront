# Browser-powered development

The developer environment downloads its terminal, npm resolver and WebAssembly compiler at
runtime. The application's C++ executable initially contains the bridge and a small bootstrap;
it does not embed React, Babel, xterm or esbuild. Node.js and npm are **not required on the
application developer's machine**. A C++ compiler, CMake and the selected native webview remain
necessary. TypeScript/JSX is transformed in the browser; type checking is not performed.

This is an experimental, explicit development facility. The downloaded environment runs with
the application's bridge authority and may read/write the selected project. It exposes no shell.
Do not ship developer mode as the distribution configuration.

The 0.4 presentation update prepares developer distribution **0.4.0** with the revised Life text.
It is not published as part of the repository cleanup. Before releasing this branch, publish the
prepared archives using [the package release procedure](../js/developer/tools/PUBLISHING.md).
For local review, provision the generated archive as described below and pass its URL/integrity
to `WebFrontLifeDeveloper`. Existing published versions remain unchanged.

## Minimal C++ application

Enable the platform's native frontend in CMake: `WEBFRONT_EMBED_WKWEBVIEW=ON` on macOS,
`WEBFRONT_EMBED_WEBVIEW2=ON` on Windows, or `WEBFRONT_EMBED_WEBKITGTK=ON` on Linux.
The default `webfront::WebFront` still opens the system browser.

```cpp
#include <webfront/WebFront.hpp>
#include <bitset>

std::bitset<1024> nextLife(const std::bitset<1024>& cells); // Existing C++ simulation.

int main() {
    webfront::NativeApp app{"."}; // Explicit development project directory.
    app.cppFunction("life.step", &nextLife);
    app.bootstrap("developer"); // Uses the version and SHA-512 pinned in WebFront.
    app.show();
}
```

The [versioned distributions](https://ambroise-leclerc.github.io/WebFront/) are published on GitHub Pages.
Version 0.3.0 provides the packed-bitset Life starter and requires the bridge support from #289.
Version 0.3.1 makes `wf install` keep the versions of the installed lock and reject an archive whose integrity differs from it.
Published versions 0.1.0 to 0.3.0 remain immutable; 0.2.0 used string-based Life grids.
On 2026-10-20, the 0.3.0 public download/WASM/React/PI/Life path passed in WKWebView, including
128-byte grid exchange, painting cells across an octet boundary, playback/reset and deterministic
header generation. Both resulting headers were compiled and the applications passed offline
native tests from empty working directories, with developer callbacks absent. Pages returned
HTTP 200, CORS `*` and the pinned archive bytes. These checks supplement offline CTest fixtures.

The default configuration is generated in `include/webfront/system/DeveloperReleaseData.hpp` and targets
`https://ambroise-leclerc.github.io/WebFront/developer/0.4.1/environment.tgz`. This version, published and archived as `developer-v0.4.1`, generates providers that include `<webfront/system/FileSystem.hpp>`. Published 0.4.0 stays immutable but targets the former include layout. The configuration pins one version and
its SHA-512 rather than resolving a mutable `latest` channel. An explicit
`DeveloperBootstrapOptions{.environment = url, .integrity = trustedHash}` still selects a different
HTTPS/CORS repository or a same-origin test fixture. Users install neither npm nor Node.js.

The environment archive contains pinned Life and PI starter descriptors. `wf create life` downloads and
verifies that separate archive, creates a new `ui` directory and prints the install/preview/embed
commands. Existing directories are never replaced. The trusted environment hash protects both
its own code and the starter descriptor; `provision.json` on the server is not a new trust anchor.
See [publication instructions](../js/developer/tools/PUBLISHING.md) for deployment and release checks.

The bootstrap verifies SHA-512 before unpacking or executing the archive, validates its manifest
and paths, then publishes it under the application's local origin. The cached **archive** is
verified again before reuse, allowing subsequent launches without network access. Failure leaves
a readable status and Retry button. A different pinned hash requires a matching archive; the old
environment is not silently substituted.

## The Game of Life walkthrough

`examples/life/Life.hpp` is a small C++23 B3/S23 simulation on a `std::bitset<1024>`.
Each generation reads the old grid and creates a new one; the 32 × 32 world wraps on both axes.
The core needs no React, server, thread, timer or graphics dependency. `nextLife` is registered directly
with its `const LifeGrid&` argument and bitset result. React stores and exchanges a 128-byte `Uint8Array`,
with cell `i` in bit `i % 8` of byte `i / 8`. The bridge validates the exact unsigned-byte type and length.
`examples/life/LifeConsole.cpp` runs it without WebFront; build and run `WebFrontLifeConsole` to start
with a plain console application.

`examples/life/LifeDeveloper.cpp` adds the native window and downloaded developer bootstrap. Configure
`build-native` with the native frontend option above, then [prepare the local archive](#environment-distribution-and-offline-tests-maintainers) and start with a fresh project directory:

```sh
mkdir -p life-project/.webfront/developer/preview/provision
cp build-developer-environment/*.tgz life-project/.webfront/developer/preview/provision/
integrity=$(node -p "require('./build-developer-environment/provision.json').integrity")
./build-native/examples/WebFrontLifeDeveloper "$PWD/life-project" \
  /webfront/developer/preview/provision/environment.tgz "$integrity"
```

Optional extra arguments `<environment-url> <sha512-integrity>` override the pinned default.
In the downloaded terminal:

```text
wf create life
wf install react@19.3.1 react-dom@19.3.1
wf preview ui/main.tsx
wf embed ui/main.tsx --output generated/LifeUI.hpp
```

The starter contains editable TSX/CSS and an exact-version `webfront.project.json` recipe.
`wf create life another-directory` selects a different new destination; existing directories
are never replaced. React DOM pulls scheduler transitively and checks its React peer.
Edit the local files in your normal editor, then run `wf preview` again. Ctrl+C interrupts
commands, including downloads and builds.

The interface draws an isometric field with raised living cells, soft lighting and translucent
panels. Play/pause, single step, tempo, glider/blinker/galaxy seeds, clear, cell painting and a plan
view make the simulation interactive. Generation and population counters expose its state.
Every step awaits `webFront.cppFunction('life.step')(cells)`; no Life rules run in JavaScript.
Only one generation request can be outstanding. Replacing a pattern or clearing the field stops
animation and discards stale replies. Errors stop playback and appear in the interface.
Canvas rendering has an accessible description; patterns and simulation controls work with the
keyboard. Individual canvas cells are pointer-editable, without keyboard cell navigation.
No fonts, images, 3D library or additional UI framework are downloaded.

![The native Game of Life demo at generation 26](images/life-preview.jpg)

`wf embed` builds JS/CSS/resources, creates an HTML entry page, preserves dependency notices and
writes a header-only `webfront::fs::LifeUI` provider using deterministic gzip. The filename stem
determines the C++ provider name. The header is replaced only after all chunks arrive successfully.
The distribution form has no project root or developer callbacks:

```cpp
#include <webfront/WebFront.hpp>
#include "Life.hpp"
#include "generated/LifeUI.hpp"

int main() {
    webfront::NativeAppWithAssets<webfront::fs::LifeUI> app;
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.show();
}
```

Reconfigure the native build with
`-DWEBFRONT_LIFE_ASSETS=/absolute/path/to/life-project/generated/LifeUI.hpp`, build target
`WebFrontLifeEmbedded` and run it. Recompilation is explicit. The resulting binary needs neither
project sources, downloaded cache nor network.

The PI starter remains available through `wf create pi` for existing projects and regression
coverage. Its sources are maintained under `js/developer/tools/starters/pi/`; the public C++
examples are limited to Hello World and Life. Existing embedded React/Babel providers remain supported.

## Dependencies, storage and limitations

- `wf install` accepts exact versions, registry tags and semver ranges, resolves ordinary
  transitive dependencies, and retains separate nodes for different versions. Required peers must
  be satisfied by compatible explicitly installed roots or direct dependencies; conflicts fail.
- Package installation scripts, native add-ons and Node.js APIs are unsupported. Packages must be
  usable in a browser. Optional dependencies are not installed in this first version; imports of
  missing dependencies fail during the build. This is not a replacement for the complete npm CLI.
- Browser/production exports, ESM, CommonJS, TS/TSX, JSX, CSS and imported resource files are supported.
  Private package exports, network imports and unavailable Node built-ins fail. Package-specific
  build pipelines and automatic polyfills are not supplied.
- `webfront-packages.lock.json` records exact package versions, dependency edges, source integrities
  and requested roots. Commit it and the source TSX/CSS; the generated provider can also be committed.
  Preview/embed consume the locked local files without resolving versions again.
- Unpacked packages are cached by name, version and integrity under `.webfront/developer/packages/`.
  A failed install can leave unused cached packages, but the lock file remains unchanged. The tools
  installed by the existing `ToolManager` keep their separate store and `webfront-tools.lock.json`.
- Published environment/preview resources are served under `/webfront/developer/`. The protected
  `/WebFront.js` remains the library's version. Sources, staging and cached archives are not HTTP
  routes. Project reads and publications reject traversal and escaping symlinks. This does not
  isolate downloaded tools or defend against concurrent modifications by another local process.
- Transfers use sequential 256 KiB chunks. Downloads are bounded to 64 MiB compressed; unpacking,
  dependency source graphs and individual publication transactions to 256 MiB, with 512 packages
  and eight active publications at most. A publication untouched for ten minutes (a reloaded or disconnected browser) is
  abandoned, and staging left by a crashed process is removed after an hour. Browser builds time out after 120 seconds.
- Installing another package keeps every locked version that still satisfies a requested range and fails if its
  archive no longer matches the recorded integrity; only new ranges are resolved. Review the resulting lock
  changes. Restoring packages from a lockfile on a fresh machine is not yet a separate command.

## Environment distribution and offline tests (maintainers)

These commands require Node/npm **only for environment maintainers**. They prepare a tarball that
application developers can download. No command publishes anything to a registry automatically.

```sh
npm ci --prefix js/developer/vendor --ignore-scripts
node js/developer/tools/build.mjs build-developer-environment --refresh-bootstrap
node js/developer/tools/fixtures.mjs build-developer-environment
```

`build.mjs` bundles the environment and compiler, includes esbuild.wasm, xterm styles and licenses,
and writes deterministic `environment.tgz`, `life-starter.tgz`, `pi-starter.tgz`, `provision.json` and
`DeveloperReleaseData.hpp`. The version and public base URL are in `js/developer/tools/distribution.json`. Dependencies are
pinned in `js/developer/vendor/package-lock.json`. `--refresh-bootstrap` additionally refreshes the two
checked-in JS bundles, their input checksums and the embedded bootstrap headers. Without that flag,
it writes only to the output directory. The archives have the package identifiers `@webfront/developer-environment`,
`@webfront/life-starter` and `@webfront/pi-starter`. They are distributed as static GitHub Pages downloads; no npm publication or
scope ownership is required. Node/npm is only used by maintainers to build them.

To try the local archive without an HTTPS server, after building the packages above:

```sh
mkdir -p life-project/.webfront/developer/preview/provision
cp build-developer-environment/*.tgz life-project/.webfront/developer/preview/provision/
integrity=$(node -p "require('./build-developer-environment/provision.json').integrity")
./build-native/examples/WebFrontLifeDeveloper "$PWD/life-project" \
  /webfront/developer/preview/provision/environment.tgz "$integrity"
```

The preview command later replaces that directory, but the verified archive is already cached
independently for subsequent launches. These are maintainer provisioning commands; the application's
terminal still installs and builds browser dependencies without a local Node/npm installation.

Fixture provisioning packs the pinned React, React DOM and scheduler sources from the maintainer's
npm installation into a fake local registry. No CTest case downloads from the public registry.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WKWEBVIEW=ON \
  -DWEBFRONT_WEBTEST_FRONTEND=WKWebView -DENABLE_TESTING=ON \
  -DWEBFRONT_DEVELOPER_FIXTURES="$PWD/build-developer-environment"
cmake --build build-native --parallel
ctest --test-dir build-native -R 'Developer|LifeEmbedded' --output-on-failure
```

The GitHub CI prepares these fixtures for both WKWebView and CEF jobs. The initial 0.2.0 Life version passed
WKWebView and all 14 Linux CEF/Xvfb cases with both GCC and Clang in
[run 39421265580](https://github.com/ambroise-leclerc/WebFront/actions/runs/39421265580).
The packed 0.3.0 version passes WKWebView and all 14 GCC CEF/Xvfb cases in
[run 39490581423](https://github.com/ambroise-leclerc/WebFront/actions/runs/39490581423).
For Linux, select CEF and Xvfb instead; the same developer tests use the selected test frontend.
`DeveloperBrowserIntegration` exercises the real WASM compiler, local registry, PI and Life previews, C++ generation/animation and
repeatable header generation. Its CTest fixtures then build that **browser-generated** header and run
`DeveloperEmbeddedIntegration` and `LifeEmbeddedIntegration` from empty working directories
with no developer callbacks.
The standard CEF-off suite also checks project transactions, path rejection, PI, Life rules and boundaries, MIME types,
bootstrap freshness, package resolution, gzip generation, interruption and offline cache reuse.
