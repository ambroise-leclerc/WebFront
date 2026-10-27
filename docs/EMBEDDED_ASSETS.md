# Embedded assets

The C++ library embeds the browser bridge (`WebFront.js`), the optional DOM interpreter (`webfront-dom.mjs`), the
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
header, a hidden payload and an oversized resource. `du -sh include/webfront/system` was about 2.5 MB when the epic began;
the guard reports current sizes.

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
with CRLF line ends is stale). It assembles bounded lines and joins them once, avoiding a growing-string CMake loop: the seven
core assets take a fraction of a second. Generation and checking share one renderer, without intermediate files.
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

fflate (MIT) and esbuild are locked packages of the developer environment (`webfront-packages.lock.json`), not vendored
sources; fflate is not embedded in C++ applications. Package notices travel with the packages that `wf install` downloads.

## Optional DOM interpreter

A page that receives DOM batches loads the version-matched module explicitly:

```js
const { installDomInterpreter } = await import('/webfront-dom.mjs');
const dom = installDomInterpreter(webFront);
const release = dom.island('panel', document.getElementById('panel')); // empty element
// Call release() before the page takes back the subtree.
```

Register C++ callbacks `wfDom.rejected(batchId, reason)` and, when listening to events,
`wfDom.event(nodeId, nameIndex, value)`. The low-level `msg::DomBatch` codec and
`ui.sendDomBatch(batch)` implement [ADR-003](adr/ADR-003-protocole-binaire-et-bornes.md);
the higher-level document/node API is described in [DOM.md](DOM.md). The default filesystem protects both
`WebFront.js` and `webfront-dom.mjs` from document-root overrides.

The interpreter accepts trusted application commands, as [ADR-008](adr/ADR-008-contenu-de-confiance-et-origines.md)
specifies. `style` may load CSS resources or position overlays, and `id`/`name` can introduce named
DOM properties. The allowlist does not isolate untrusted producers. The name dictionary is retained
until the document is discarded, including definitions acquired from rejected batches; it has a
1 MiB UTF-8 budget in addition to the entry and per-name limits. Low-level C++ callers manage its
mirror; the `Document` helper manages its own mirror (see [DOM.md](DOM.md)). After semantic rejection, retry the corrected
operations using the names already acquired, without repeating their NAME definitions. Redefining
an acquired index is rejected.

`data:` URLs are rejected even for `<img src>`; serve an image from an embedded application provider
or a development document root and use its relative HTTP URL instead.

C++ batches require an explicit nonzero identifier (`msg::DomBatch(id)`); zero is reserved for
uncorrelated malformed-message rejections. Both codecs enforce a limit of 131,072 operations per
batch as well as the 1 MiB byte limit. The owning C++ decoder reserves the counted operation total;
its object memory still scales with that count. Invalid formats, UTF-8 and unused nonzero operands
throw `std::invalid_argument`; byte/operation budget violations throw `std::length_error`.

A residual DOM exception invalidates all node handles and island registrations. Existing release
functions then do nothing. C++ receives `wfDom.rejected` and must arrange page recovery: explicitly
empty the former island elements (`replaceChildren()`) or replace them before lending empty elements
again. Names remain interned. Reload the document to reset names as well. See ADR-003 for the exact
`wfDom.event` value contract.

`PROPERTY value` is rejected on `<output>`, including empty elements, because its native setter
replaces child nodes. Create a text child and use `SET_TEXT` to update the displayed value.

## Manual small-batch measurement

`test/web/dom-benchmark.html` measures repeated single-operation SET_TEXT batches against detached
flat DOM trees with 101, 10,001 and 60,001 live nodes. SET_TEXT repeatedly assigns `x` to the same
existing text node. Each size warms up with 20 batches, then takes
three samples of 100 batches; the displayed result is the median time per batch. It includes decoding,
validation and DOM text mutation, and excludes transport, layout and rendering. It is not a CTest case
and asserts no timing threshold. To run from the repository root with optional Python tooling:

```sh
measure_dir="$(mktemp -d)"
cp test/web/dom-benchmark.html "$measure_dir/index.html"
cp js/core/dom-interpreter.mjs "$measure_dir/webfront-dom.mjs"
python3 -m http.server 8000 --bind 127.0.0.1 --directory "$measure_dir"
# Open http://127.0.0.1:8000/ in a browser and click Measure.
```

Local Chrome 156/macOS observation on 2026-10-27, baseline `fe7e23b` versus the transactional delta
implementation (milliseconds per batch, median of three samples):

| Live nodes | Whole-table copy | Transactional delta |
| --- | ---: | ---: |
| 101 | 0.095 | 0.007 |
| 10,001 | 8.628 | 0.007 |
| 60,001 | 10.627 | 0.043 |

These short local measurements are sensitive to JIT, garbage collection, timer resolution and host
load; they are not a cross-engine guarantee. The deterministic Jasmine regression separately counts
Map/Set iterator visits to ensure a small text/append batch does not scan unrelated nodes or siblings.
