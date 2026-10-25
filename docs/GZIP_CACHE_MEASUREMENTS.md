# Gzip fallback cache measurements (#281)

Measured on Linux x86-64, GCC 15.2.0, Release (`-O3 -DNDEBUG`), 2026-10-25.
Both executables enable `_GLIBCXX_ASSERTIONS` and `-ftrivial-auto-var-init=pattern`.
These are local timing observations, not performance assertions or cross-platform guarantees.
The benchmark calls the real `RequestHandler` with an in-memory compressed provider: URI
parsing, provider reads, negotiation, decoding/cache lookup and response construction are
included; network I/O, browser decompression and filesystem latency are excluded.

The before executable uses `HTTPServer.hpp` from commit `642a435`, with a counter added at
its `decodeGzip` call. The after executable uses the new cache and its decode-attempt and
retained-byte counters. Each row performs **200 requests**. Repeated rows use one handler
and include its first cold request; cold rows create a fresh handler for every request.
The optional target also runs the new handler with retention disabled as an uncached control.
No decoder algorithm or gzip validation change is included in #281.

The neutral fixed/dynamic/optional fixtures are from `test/unit/GzipFixtures.hpp`. The
application input is the generated developer compiler bundle `build-distribution/compiler.mjs`,
compressed with Python's `gzip.compress(bytes, mtime=0)`. It is an application bundle,
not a restored core provider. Its SHA-256 is listed below so regenerated bundle changes
cannot silently be mistaken for the same workload.

## Sizes and retained memory

| Asset | Compressed bytes | Decoded bytes | Before retained bytes | After retained bytes |
| --- | ---: | ---: | ---: | ---: |
| fixed | 499 | 23768 | 0 | 24357 |
| dynamic | 453 | 23768 | 0 | 24311 |
| optional | 476 | 23768 | 0 | 24334 |
| application | 22902 | 78508 | 0 | 101500 |

Retained bytes include both string capacities and terminators, entry metadata and list links
(90 bytes of accounted overhead per entry on this libstdc++ build). GET and HEAD retain the
same entry. This measures cache retention, **not peak process memory**: provider/input bytes,
response buffers, decoder temporaries and allocator bookkeeping are outside these figures.
Every cold request still allocates a bounded decoded representation; admission temporarily
constructs a retained copy. An evicted/disabled/oversized entry retains zero bytes.

## Timing and decode counts

Before: **200 decodes** in every row. After: **1 decode** for repeated requests and
**200 decodes** for cold requests. With cache retention disabled: **200 decodes**, zero
retained bytes. Tests separately verify mixed GET/HEAD reuse and a smaller active output
budget, content replacement, invalid streams, admission limits, eviction and shutdown.

| Asset | Method | Repeated before (µs/request) | Repeated after | Cold before | Cold after |
| --- | --- | ---: | ---: | ---: | ---: |
| fixed | GET | 74.60 | 2.30 | 76.30 | 76.14 |
| fixed | HEAD | 75.14 | 1.83 | 77.19 | 76.45 |
| dynamic | GET | 75.11 | 2.27 | 76.58 | 75.71 |
| dynamic | HEAD | 75.22 | 1.81 | 76.17 | 75.75 |
| optional | GET | 75.80 | 2.28 | 76.52 | 76.40 |
| optional | HEAD | 75.21 | 1.82 | 75.57 | 76.17 |
| application | GET | 921.80 | 32.54 | 934.22 | 916.78 |
| application | HEAD | 907.11 | 31.15 | 894.37 | 913.72 |

The improvement comes from eliminating repeat validation, including CRC32 and DEFLATE.
Cold application decoding remains about 0.9 ms here, on the I/O thread; #283 must address
that occupancy. The warm application cost still includes reading and comparing 22.9 KiB of
compressed bytes; #282 can use the cold rows as a decoder baseline. Small timing differences
between cold before/after runs include allocation and host scheduling noise.

## Reproduction

With the normal CEF-off test build configured:

```bash
cmake --build build --target gzip_cache_benchmark --parallel 2
# Without an argument, runs all three neutral fixtures.
build/test/unit/gzip_cache_benchmark
# Optionally use a generated application bundle, or any complete gzip asset under 4 MiB decoded.
python3 -c 'import gzip,pathlib; pathlib.Path("/tmp/wf-compiler.mjs.gz").write_bytes(gzip.compress(pathlib.Path("build-distribution/compiler.mjs").read_bytes(), mtime=0))'
build/test/unit/gzip_cache_benchmark /tmp/wf-compiler.mjs.gz
```

The CSV reports sizes, iterations, actual decode attempts, retained bytes and mean time per
request for both enabled and disabled retention. It is excluded from the normal build and
CTest; no timing threshold is imposed. Re-run on the target compiler/platform before using
the timings to justify decoder changes. For a historical before comparison, compile the same
harness against the pre-cache `RequestHandler` and count its `decodeGzip` calls.

Application bundle SHA-256: `5f36728943433f1b88eadcdc0db910133257b4502c8faec3357168b6739f70bc`.
