# Embedded browser dependencies

The following npm `dist-tags.latest` values were checked against registry.npmjs.org on
2026-10-19. All application and test resources are served locally; no runtime CDN is used.
Normal C++ consumer builds use checked-in headers and need neither Node.js nor npm.

| Package | Exact version | Source and served distribution |
| --- | --- | --- |
| `@xterm/xterm` | 6.0.0 | npm `lib/xterm.mjs`, `css/xterm.css`; unchanged upstream bytes |
| `@xterm/addon-fit` | 0.11.0 | npm `lib/addon-fit.mjs`; unchanged upstream bytes |
| `react` | 19.3.1 | npm production CommonJS, bundled into the local ESM runtime below |
| `react-dom` | 19.3.1 | npm client entry, sharing the same React instance |
| `@babel/standalone` | 8.0.7 | npm `babel.min.js`; unchanged upstream bytes |
| `jasmine-core` | 7.0.2 | npm `lib/jasmine-core/{jasmine.js,jasmine-html.js,jasmine.css,boot.js}` and `images/jasmine_favicon.png` |

React 19 removed UMD builds and requires the automatic JSX transform
([upstream upgrade guide](https://react.dev/blog/2024/04/25/react-19-upgrade-guide)).
`js/developer/vendor/update.mjs` bundles React, React DOM and `react/jsx-runtime` together as
`/react@19.3.1/runtime.mjs`. This keeps hooks, DOM rendering and JSX on one React instance.
The import map in `test/web/fixtures/react.html` maps `react`, `react-dom/client` and `react/jsx-runtime`
to that runtime. Babel's custom `webfront-react` preset uses `runtime: "automatic"` and
emits a native module. The JSX remains editable in the HTML file, without a consumer build step.
Babel is served at `/babel-standalone@8.0.7/babel.min.js`.

Jasmine 7 boots its globals from `jasmine.js`; its single `boot.js` installs the HTML reporter
and starts the suite on window load. The runner serves `/jasmine/7.0.2/` and awaits the final
C++ reporting Promise. The favicon is served as its original PNG, without an extra gzip layer.
The terminal keeps its existing `webfront/components/terminal/` URLs and MIME types.

## Maintenance

Use Node.js 22.18 or newer (Node 24 in CI), npm and CMake 3.31 or newer. These tools are needed
for regeneration and `ENABLE_TESTING=ON` asset checks, not for normal library consumers.
Run from the repository root:

```sh
# Online, maintenance only: exact versions and transitive integrity hashes are in package-lock.json.
npm ci --prefix js/developer/vendor --ignore-scripts
node js/developer/vendor/update.mjs
# Refresh xterm only through its dedicated script, retaining readable upstream files and licenses.
cmake -DXTERM_VERSION=6.0.0 -DFIT_VERSION=0.11.0 -P cmake/UpdateXterm.cmake

# Offline after C++ dependencies are configured; npm/node_modules is not required here.
cmake --build build --target webfront-embedded-assets
ctest --test-dir build -R 'Asset.*Fresh|EmbeddedGzipContents|EmbeddedAssetGenerator' --output-on-failure --parallel 4
```

`js/developer/vendor/package.json` pins all six library versions and the maintenance tools;
`package-lock.json` pins the npm tarball URLs, integrity hashes, scheduler 0.28.0 and esbuild 0.28.2.
When upgrading, recheck the registry's stable tags, update exact versions and the lockfile,
then review sources, routes, tests, version inventory and size measurements together.
`cmake/UpdateXterm.cmake` takes exact versions explicitly and records them in
`third_party/xterm/VERSION`. `js/developer/vendor/update.mjs` records the other versions alongside
their distribution files in `third_party/`.

`js/core/EmbedTextAsset.cmake` delegates rendering and exact byte comparison to
`js/developer/vendor/embed.mjs`. It assembles bounded lines of five 64-bit words and joins them
once, avoiding the growing-string CMake loop. Generation and checking use the same
renderer, without intermediate files. `EmbeddedAssetGenerator` covers empty/binary inputs,
word boundaries, padding, line wrapping and nonzero status for stale or missing files;
all per-asset freshness checks and the independent gzip decoder remain enabled.

Compression uses the vendored **fflate 0.8.3** pure JavaScript `gzipSync` implementation
(`third_party/fflate/browser.mjs`), with **level 9, mem 8, mtime 0**, no filename and a fixed
Unix OS header byte. It never uses the host's gzip or zlib compressor. The compressor and
its license are checked in, so the streams are identical across platforms without an npm install.
The small bridge, fallback page and component modules remain identity encoded.
`.gitattributes` keeps text sources and headers at LF; PNG files retain their binary bytes.

The renderer packs the compressed bytes into big-endian words, with
`encoding = "gzip"` and `dataSize` excluding trailing word padding. CTest regenerates and compares
headers offline. `js/developer/vendor/check.mjs` independently decodes the arrays using Node's zlib,
checks every decoded byte against the distribution file, validates gzip headers/padding, and
checks React's named ESM exports. Catch2 exercises provider routes, reads across word boundaries,
EOF, encoding metadata and HTTP Content-Encoding/Content-Length/MIME/body agreement.
The shared Jasmine suite drives the actual React example and xterm/Fit resizing locally.

Encoding negotiation includes the identity fallback from #274. Clients accepting gzip receive
the stored gzip representation; other clients accepting identity receive decoded bytes with
their decoded Content-Length. HTTP regression tests compare every dependency JS/CSS fallback
with its vendored source, including absent Accept-Encoding, explicit identity and gzip;q=0.
See [HTTP asset encoding](HTTP_ASSETS.md) for negotiation, decoding limits and failure responses.
Decoder caching and performance remain separate work in #281–#283.

## Licenses

MIT notices are retained in `third_party/{react,babel,jasmine,xterm,fflate}/` and in upstream
payloads. React DOM and scheduler notices are also preserved beside the React runtime.
fflate and esbuild are maintenance tools; fflate is not embedded in C++ applications.

## Size measurements

The measurements below predate the 0.4 example cleanup; removed target names identify historical binaries. Current build commands use the retained examples.

The measurements below predate the integration of #274 and isolate the asset refresh from
its HTTP decoder changes. They describe the original #284 implementation, not current binary
sizes after rebasing onto the identity-fallback work.

Measured on macOS arm64 with Homebrew Clang 21.1.8, Ninja, C++23, Release `-O3 -DNDEBUG`,
CEF disabled, tests enabled, cache disabled, no stripping. Baseline: commit `838f0de`.
All three builds used the same source path, compiler, architecture and CMake options.
The middle build changed only xterm's three generated payloads to gzip and retained the
original React/Babel/Jasmine providers. Headers were restored before final validation.
Executable sizes include Mach-O alignment and the existing test hardening options.

```sh
cmake -S . -B build-measure -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/path/to/clang++ -DWEBFRONT_EMBED_CEF=OFF \
  -DENABLE_TESTING=ON -DENABLE_CACHE=OFF
cmake --build build-measure --target HelloWorld WebFrontLifeDeveloper webtest --parallel 2
# On macOS with standalone LLVM, also pass -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path).
# Compare file sizes, not CEF frameworks or other external engine resources.
```

Array storage in bytes, **including 64-bit word padding**:

| Resources | Original | Only xterm gzip | Old versions, all gzip with fflate | Final versions |
| --- | ---: | ---: | ---: | ---: |
| xterm + CSS + Fit | 354,056 | 95,272 | 95,272 | 95,272 |
| React + React DOM | 47,224 | 47,224 | 49,144 | 72,928 |
| Babel | 596,736 | 596,736 | 615,184 | 563,592 |
| Jasmine (including PNG) | 79,536 | 79,536 | 81,568 | 95,328 |
| **Total** | **1,077,552** | **818,768** | **841,168** | **827,120** |

xterm's gzip migration alone saves **258,784 bytes** of array storage with identical decoded
resources. Recompressing the older gzip payloads with fflate accounts for a 22,400-byte
increase; upgrading/rebundling on that compressor then saves 14,048 bytes, including the
switch to a raw PNG favicon. The final total is 8,352 bytes above the xterm-only gzip build,
and **250,432 bytes below the original**. Major upgrades therefore do not uniformly shrink
resources: React's combined DOM/JSX runtime and Jasmine grow while Babel shrinks.

Exact served lengths (without padding) are printed by `node js/developer/vendor/check.mjs`:
xterm module 91,908; CSS 2,547; Fit 807; React runtime 72,925; Babel 563,586;
Jasmine JS 75,638; HTML reporter 9,547; CSS 7,568; boot 1,073; PNG 1,486 bytes.

Release executable file sizes in bytes:

| Target | Original | Only xterm gzip | Final versions |
| --- | ---: | ---: | ---: |
| `HelloWorld` | 941,464 | 941,464 | 941,464 |
| `WebFrontGreeter` | 950,960 | 950,960 | 950,960 |
| `WebFrontApp` | 1,654,456 | 1,654,456 | 1,637,848 |
| `WebFrontTerminal` | 1,461,824 | 1,197,632 | 1,197,632 |
| `WebFrontTools` | 1,702,192 | 1,454,512 | 1,454,512 |
| `webtest` | 3,033,544 | 2,785,864 | 2,785,704 |

`HelloWorld` and `WebFrontGreeter` remain byte-for-byte identical (SHA-256 checked), confirming
that minimal examples do not acquire unused terminal, React, Babel or Jasmine providers.
`webtest` here is the CEF-off diagnostic executable; enabled engine binary sizes are not
comparable to this measurement.
