# HTTP asset encoding

HTTP asset negotiation and fallback apply to every frontend and to custom filesystem providers.

The embedded Jasmine, React, Babel and xterm JavaScript/CSS payloads use gzip.
The Jasmine favicon is served as its original PNG. If a client does not
accept gzip, the HTTP server decodes the asset and serves identity with the decoded `Content-Length` and no
`Content-Encoding`. Encoded responses carry `Vary: Accept-Encoding`; explicit `q=0`
exclusions are respected. Duplicate mentions accept a coding if any valid mention accepts it,
regardless of their order or which field contains them: `gzip;q=0, gzip` and `gzip, gzip;q=0`
both accept gzip. Coding names use locale-independent ASCII case folding.
`x-gzip` is an alias of `gzip` in preferences and stored assets, as specified by
[RFC 9110 §8.4.1.3](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.4.1.3).
Alias mentions share the same explicit preference, including exclusions and duplicate merging;
pass-through retains the stored coding label.
Explicit coding mentions still override `*`. Unknown parameters are ignored;
malformed qvalues, including `q=.5` (use `q=0.5`), reject only that mention.

Decoding validates gzip headers, DEFLATE blocks, CRC32 and uncompressed sizes, including
concatenated members, with a 4 MiB default total output limit.
Any trailing bytes after the final member, including zero padding, are rejected.
Fallback decoding failures or an exceeded limit return HTTP 500. If neither the stored
coding nor identity is acceptable, or a custom provider uses an unsupported coding
(such as Brotli) the client does not accept, the server returns HTTP 406. Custom codings
can still be served directly to accepting clients.

Fallback decoding runs synchronously on the server I/O thread and is repeated for each
request, including HEAD to validate the stream and compute its decoded length. Decoded
assets are not cached. Stored DEFLATE blocks are copied in bulk and match copies grow the
output once per match; Huffman decoding still reads individual bits. Follow-ups track
[validated asset caching and HEAD metadata](https://github.com/ambroise-leclerc/WebFront/issues/281),
[buffered bit reads and benchmarks](https://github.com/ambroise-leclerc/WebFront/issues/282), and
[cold decoding off the I/O thread](https://github.com/ambroise-leclerc/WebFront/issues/283).

Gzip remains the embedded format so fallback needs no compiled dependency or Brotli dictionary.
Pinned versions, reproducible generation and current stored/decoded resource checks are
documented in [Embedded browser dependencies](EMBEDDED_ASSETS.md). Returning them to Brotli
would require a separate decoder dependency and is deferred.

## Fallback budget

The limit applies to the sum of decoded bytes across all gzip members, for GET and HEAD.
Gzip pass-through to an accepting client does not use this budget. A smaller budget reduces
the work and memory a fallback request can consume, but it is not an I/O-thread time limit.
All bundled assets fit under the 4 MiB default; custom applications can override it through
their existing buffer policy:

```cpp
#include <WebFront.hpp>
#include <cstddef>

struct AppBuffers : webfront::http::DefaultBuffersPolicy {
    static constexpr std::size_t maxDecodedAssetSize = 1024 * 1024;
};
using App = webfront::BasicWF<webfront::NetProvider, webfront::fs::DefaultFS, AppBuffers>;
```

`CustomBuffersPolicy` also accepts the limit as its fifth template argument. Policies without
`maxDecodedAssetSize` inherit the default. Zero permits only empty decoded assets. An asset
exceeding a smaller custom budget returns 500 when identity is needed; gzip pass-through is
unchanged. The follow-ups above address caching, decoder throughput and I/O-thread occupancy.
