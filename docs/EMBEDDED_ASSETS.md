# Embedded assets

The C++ library embeds only what every application needs: the browser bridge (`WebFront.js`), the
fallback page and module, and the bootstrap page and loader of the developer environment, with the
pinned release data (`Developer*Data.hpp`). Each is generated from a readable source under `js/core/`
(including the default favicon, `js/core/favicon.ico`) into a header under `include/webfront/system/`. Libraries such as React, Babel and Jasmine are not
embedded: they are npm packages that `wf install` locks and downloads (see
[DEVELOPER_ENVIRONMENT.md](DEVELOPER_ENVIRONMENT.md)), and the tests obtain them from a local registry.
Normal C++ consumer builds use the checked-in headers and need nothing but the compiler: the headers are generated, and checked, in pure CMake.

## Migration

`BabelFS`, `ReactFS` and `JasmineFS` (and `BabelData.hpp`, `ReactRuntimeData.hpp`, `Jasmine*Data.hpp`)
were removed. An application that served them replaces them with `wf install` of the package it needs
(`react`, `react-dom`, `@babel/standalone`, `jasmine-core`) and `wf embed`, which generates a
header-only provider for the application's own build output.

## Guard

The `MinimalCoreAssets` CTest case (`test/cmake/CheckMinimalCoreAssets.cmake`, pure CMake) lists every
resource header of `include/webfront/` (named `*Data.hpp` or declaring a `dataSize` payload) and fails on one
outside the normative list of [ADR-016](adr/ADR-016-bootstrap-comme-socle.md), or when the decoded payloads exceed
150 KiB. The budget counts decoded bytes (gzip streams are measured by their trailer), so it does not depend on the
encoding; the test also prints the encoded bytes and the size of the headers. Adding a resource means amending the ADR
and the list in the script together. `MinimalCoreAssetsRejectsUnlisted` proves the guard fails on an unlisted
header, a hidden payload and an oversized resource. `du -sh include/webfront/system` was about 2.5 MB when the epic began
and is 188 KB now.

## Maintenance

CMake 3.31 or newer is the only tool: regeneration and the asset checks of `ENABLE_TESTING=ON` are CMake scripts, and
normal library consumers need neither. From the repository root:

```sh
# Regenerate the headers from their sources, then verify them offline.
cmake --build build --target webfront-embedded-assets
ctest --test-dir build -R 'Asset.*Fresh|EmbeddedAssetGenerator|BundleInputs|embedded assets of the core' --output-on-failure --parallel 4
```

`js/core/EmbedAsset.cmake` renders a header (identity encoded, five 64-bit words per line, big-endian, the last word padded
with zeros, `dataSize` excluding the padding) and compares its exact bytes with a checked-in one (`-DCHECK`, so that a header
with CRLF line ends is stale). It assembles bounded lines and joins them once, avoiding a growing-string CMake loop: the six
core assets, about 35 KB, take a fraction of a second. Generation and checking share one renderer, without intermediate files.
`EmbeddedAssetGenerator` (`test/cmake/CheckEmbedAsset.cmake`) covers the exact format, empty and binary inputs, word
boundaries, padding, a path with spaces, a checkout reached through a symbolic link and a nonzero status for stale or missing
files; a per-asset `*AssetFresh` test attributes each stale header, and a Catch2 scenario decodes every header with the C++
data it declares and compares it with the source file. `.gitattributes` keeps sources and headers at LF. The choice of CMake
over a script run by `wf` is recorded in [ADR-016](adr/ADR-016-bootstrap-comme-socle.md): the headers must be checkable by a
build that has no browser.

The bundles of the environment (`js/core/bootstrap/bootstrap.bundle.mjs`, `js/developer/environment/packages.bundle.mjs`) are
built by `wf run build <directory> --refresh-bootstrap`. `js/developer/environment/bundles.json` records the SHA-256 of
each of their inputs (`js/developer/tools/bundle-inputs.json`) and of the bundles themselves; `BundleInputsFresh` fails
when a source changed without rebuilding them, and `BundleInputsRejectsChange` proves that it does. After a refresh, run the
`webfront-embedded-assets` target for the headers that embed the bootstrap bundle.

Applications that embed gzip-compressed resources use `wf embed`, which compresses in the environment with the locked
`fflate` (level 9, mem 8, mtime 0, no filename, fixed OS byte), so the streams are identical across platforms. Catch2 covers the serving of
gzip assets with a neutral fixture module: clients accepting gzip
receive the stored representation, other clients accepting identity receive decoded bytes with their
decoded `Content-Length`. See [HTTP asset encoding](HTTP_ASSETS.md) for negotiation, decoding limits
and failure responses.

## Licenses

fflate (MIT, `third_party/fflate/`) and esbuild are maintenance tools; fflate is not embedded in C++
applications. Package notices travel with the packages that `wf install` downloads.
