# Embedded assets

The C++ library embeds only what every application needs: the browser bridge (`WebFront.js`), the
fallback page and module, and the bootstrap page and loader of the developer environment, with the
pinned release data (`Developer*Data.hpp`). Each is generated from a readable source under `js/core/`
into a header under `include/webfront/system/`. Libraries such as React, Babel and Jasmine are not
embedded: they are npm packages that `wf install` locks and downloads (see
[DEVELOPER_ENVIRONMENT.md](DEVELOPER_ENVIRONMENT.md)), and the tests obtain them from a local registry.
Normal C++ consumer builds use the checked-in headers and need neither Node.js nor npm.

## Migration

`BabelFS`, `ReactFS` and `JasmineFS` (and `BabelData.hpp`, `ReactRuntimeData.hpp`, `Jasmine*Data.hpp`)
were removed. An application that served them replaces them with `wf install` of the package it needs
(`react`, `react-dom`, `@babel/standalone`, `jasmine-core`) and `wf embed`, which generates a
header-only provider for the application's own build output.

## Maintenance

Use Node.js 22.18 or newer (Node 24 in CI) and CMake 3.31 or newer. These tools are needed for
regeneration and `ENABLE_TESTING=ON` asset checks, not for normal library consumers. From the
repository root:

```sh
# Regenerate the headers from their sources, then verify them offline.
cmake --build build --target webfront-embedded-assets
ctest --test-dir build -R 'Asset.*Fresh|EmbeddedAssetGenerator' --output-on-failure --parallel 4
```

`js/core/EmbedTextAsset.cmake` delegates rendering and exact byte comparison to
`js/developer/vendor/embed.mjs`. It assembles bounded lines of five 64-bit words and joins them
once, avoiding the growing-string CMake loop. Generation and checking use the same renderer, without
intermediate files. `EmbeddedAssetGenerator` covers empty and binary inputs, word boundaries, padding,
line wrapping and a nonzero status for stale or missing files; a per-asset `*AssetFresh` test attributes
each stale header. `.gitattributes` keeps sources and headers at LF.

The renderer can pack gzip-compressed bytes (`encoding = "gzip"`, `dataSize` excluding the trailing word
padding) with the vendored fflate `gzipSync` (level 9, mem 8, mtime 0, no filename, fixed OS byte), so
the streams are identical across platforms. The current assets are identity encoded because they are
small. Catch2 covers the serving of gzip assets with a neutral fixture module: clients accepting gzip
receive the stored representation, other clients accepting identity receive decoded bytes with their
decoded `Content-Length`. See [HTTP asset encoding](HTTP_ASSETS.md) for negotiation, decoding limits
and failure responses.

## Licenses

fflate (MIT, `third_party/fflate/`) and esbuild are maintenance tools; fflate is not embedded in C++
applications. Package notices travel with the packages that `wf install` downloads.
