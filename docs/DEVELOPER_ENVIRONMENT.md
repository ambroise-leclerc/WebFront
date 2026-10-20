# Browser-powered development

The developer environment downloads its terminal, npm resolver and WebAssembly compiler at
runtime. The application's C++ executable initially contains the bridge and a small bootstrap;
it does not embed React, Babel, xterm or esbuild. Node.js and npm are **not required on the
application developer's machine**. A C++ compiler, CMake and the selected native webview remain
necessary. TypeScript/JSX is transformed in the browser; type checking is not performed.

This is an experimental, explicit development facility. The downloaded environment runs with
the application's bridge authority and may read/write the selected project. It exposes no shell.
Do not ship developer mode as the distribution configuration.

## Minimal C++ application

Enable the platform's native frontend in CMake: `WEBFRONT_EMBED_WKWEBVIEW=ON` on macOS,
`WEBFRONT_EMBED_WEBVIEW2=ON` on Windows, or `WEBFRONT_EMBED_WEBKITGTK=ON` on Linux.
The default `webfront::WebFront` still opens the system browser.

```cpp
#include <WebFront.hpp>

std::string pi(std::uint32_t decimals); // Existing application function.

int main() {
    webfront::NativeApp app{"."}; // Explicit development project directory.
    app.cppFunction("pi", &pi);
    app.bootstrap("developer"); // Uses the version and SHA-512 pinned in WebFront.
    app.show();
}
```

The default configuration is generated in `include/system/DeveloperReleaseData.hpp` and targets
`https://ambroise-leclerc.github.io/WebFront/developer/0.1.0/environment.tgz`. It pins one version and
its SHA-512 rather than resolving a mutable `latest` channel. An explicit
`DeveloperBootstrapOptions{.environment = url, .integrity = trustedHash}` still selects a different
HTTPS/CORS repository or a same-origin test fixture. Users install neither npm nor Node.js.

The environment archive contains a pinned PI starter descriptor. `wf create pi` downloads and
verifies that separate archive, creates a new `ui` directory and prints the install/preview/embed
commands. Existing directories are never replaced. The trusted environment hash protects both
its own code and the starter descriptor; `provision.json` on the server is not a new trust anchor.
See [publication instructions](../tools/developer/PUBLISHING.md) for deployment and release checks.

The bootstrap verifies SHA-512 before unpacking or executing the archive, validates its manifest
and paths, then publishes it under the application's local origin. The cached **archive** is
verified again before reuse, allowing subsequent launches without network access. Failure leaves
a readable status and Retry button. A different pinned hash requires a matching archive; the old
environment is not silently substituted.

## The PI walkthrough

`src/PiDeveloper.cpp` is the complete developer executable. Its small bounded decimal spigot
lives in `src/Pi.hpp`. Start with an empty project directory:

```sh
./build-native/src/WebFrontPiDeveloper /path/to/pi-project
```

The optional extra arguments `<environment-url> <sha512-integrity>` override the pinned default
for a local test or alternative distributor. In the downloaded terminal:

```text
wf create pi
wf install react@19.3.1 react-dom@19.3.1
wf preview ui/main.tsx
wf embed ui/main.tsx --output generated/PiUI.hpp
```

`wf create pi another-directory` selects a different new destination. The starter includes editable
TSX/CSS and `webfront.project.json` with exact dependency versions. Edit those files in your normal
editor; no code editor or framework needs to be embedded in the original application.

React DOM pulls scheduler transitively and checks its React peer. The TSX calls
`await webFront.cppFunction('pi')(digits)`; failures reject the Promise and are displayed.
The preview opens inside the same native window. Edit the local files in your ordinary editor,
then run `wf preview` again. Ctrl+C interrupts a browser command, including downloads or a build.

`wf embed` builds JS/CSS/resources, creates an HTML entry page, preserves dependency notices and
writes a header-only `webfront::fs::PiUI` provider. It uses deterministic fflate gzip and the
existing big-endian word format, with MIME types inferred from resource routes as usual.
The filename stem determines the provider class name and must be a C++ identifier.
The header is replaced only after all chunks have been received successfully.

The distribution form has no project root or developer callbacks:

```cpp
#include <WebFront.hpp>
#include "generated/PiUI.hpp"

webfront::NativeAppWithAssets<webfront::fs::PiUI> app;
app.cppFunction("pi", &pi);
app.show();
```

For the supplied example, reconfigure the native build with
`-DWEBFRONT_PI_ASSETS=/absolute/path/to/pi-project/generated/PiUI.hpp`, build target
`WebFrontPiEmbedded` and run it. Recompilation is performed explicitly by the developer.
The binary then needs neither the project sources, the downloaded cache nor the network.
The original embedded React/Babel providers remain supported for existing applications.

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
  and eight active publications at most. Browser builds time out after 120 seconds.
- Installing another package may re-resolve compatible transitive ranges; review the resulting lock
  changes. Restoring packages from a lockfile on a fresh machine is not yet a separate command.

## Environment distribution and offline tests (maintainers)

These commands require Node/npm **only for environment maintainers**. They prepare a tarball that
application developers can download. No command publishes anything to a registry automatically.

```sh
npm ci --prefix tools/assets --ignore-scripts
node tools/developer/build.mjs build-developer-environment --refresh-bootstrap
node tools/developer/fixtures.mjs build-developer-environment
```

`build.mjs` bundles the environment and compiler, includes esbuild.wasm, xterm styles and licenses,
and writes deterministic `environment.tgz`, `pi-starter.tgz`, `provision.json` and
`DeveloperReleaseData.hpp`. The version and public base URL are in `tools/developer/distribution.json`. Dependencies are
pinned in `tools/assets/package-lock.json`. `--refresh-bootstrap` additionally refreshes the two
checked-in JS bundles, their input checksums and the embedded bootstrap headers. Without that flag,
it writes only to the output directory. The two archives have the package identifiers `@webfront/developer-environment` and
`@webfront/pi-starter`. They are distributed as static GitHub Pages downloads; no npm publication or
scope ownership is required. Node/npm is only used by maintainers to build them.

To try the local archive without an HTTPS server, copy `environment.tgz` to the project's
`.webfront/developer/preview/provision/environment.tgz` and pass
`/webfront/developer/preview/provision/environment.tgz` as its URL. The preview command later replaces
that directory, but the verified archive is already cached independently for subsequent launches.

Fixture provisioning packs the pinned React, React DOM and scheduler sources from the maintainer's
npm installation into a fake local registry. No CTest case downloads from the public registry.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WKWEBVIEW=ON \
  -DWEBFRONT_WEBTEST_FRONTEND=WKWebView -DENABLE_TESTING=ON \
  -DWEBFRONT_DEVELOPER_FIXTURES="$PWD/build-developer-environment"
cmake --build build-native --parallel
ctest --test-dir build-native -R 'Developer' --output-on-failure
```

The GitHub CI prepares these fixtures for both WKWebView and CEF jobs.
For Linux, select CEF and Xvfb instead; the same developer tests use the selected test frontend.
`DeveloperBrowserIntegration` exercises the real WASM compiler, local registry, PI preview and
repeatable generation. Its CTest fixtures then build that **browser-generated** header and run
`DeveloperEmbeddedIntegration` from an empty working directory with no developer callbacks.
The standard CEF-off suite also checks project transactions, path rejection, PI, MIME types,
bootstrap freshness, package resolution, gzip generation, interruption and offline cache reuse.
