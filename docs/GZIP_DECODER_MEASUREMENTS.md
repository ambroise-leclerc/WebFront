# Gzip decoder measurements (#282)

Measured on macOS 27.0.1, Apple M2 arm64, Homebrew Clang 21.1.8/libc++, Release
(`-std=c++23 -O3 -DNDEBUG`), 2026-10-25. Both versions use libc++ extensive hardening
and `-ftrivial-auto-var-init=pattern`. Timings are informational; no CI timing threshold.

The baseline is `Gzip.hpp` at `d27e4fc`, after the cache (#281) and bounded worker (#283).
The same `GzipDecoderBenchmark.cpp` runs against both headers. It measures decoding,
CRC/header/trailer validation and byte-for-byte output verification, excluding provider reads,
HTTP, cache lookup, scheduling and network I/O. Every iteration decodes a cold representation.
`fresh` destroys the output string each iteration; `reserved` reserves the exact decoded size
before timing, representing the worker's preallocated output. The actual worker reserves its
configured limit. The benchmark does not simulate worker admission or response copying.

The [cache measurements](GZIP_CACHE_MEASUREMENTS.md) show that cache hits remove repeat decoding;
this change reduces the remaining cold CPU work. Warm cache hits and gzip pass-through do not
invoke the decoder and receive no algorithmic benefit. The worker still runs one active decode,
with the same output, admission and shutdown contracts.

## Corpus and results

`GenerateGzipCorpus.py` uses Python 3.14.7/zlib 1.2.12, seed 282, gzip level 6 unless
specified. Compressed hashes depend on the Python zlib version; CI regenerates its own
corpus and does not require byte-identical compression across versions. The core bridge/bootstrap
sources are compressed **test inputs**; their production representations remain identity. The application is the generated developer compiler bundle
(78,508 decoded bytes, SHA-256 `5f36728943433f1b88eadcdc0db910133257b4502c8faec3357168b6739f70bc`).
No removed core provider is restored. Near-budget cases decode to 4,194,303 bytes, one byte
below the default limit: stored uses level 0 and deterministic random bytes; fixed/dynamic
use repeating JavaScript text; Huffman-only uses a deterministic sixteen-symbol alphabet;
incompressible uses random bytes and zlib's default strategy (predominantly stored blocks).
Some near-budget inputs exceed the server's separate 4 MiB compressed-input cap; these are
codec benchmarks, not claims that the default server admits those representations.

Median of three runs, alternating order. Throughput uses preallocated output; fresh-output
times and allocation totals are shown separately. Allocation totals match the baseline.

| Asset | gzip / decoded bytes | Reserved µs before / after | MiB/s before / after | Fresh µs before / after | Fresh allocations / bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| fixed | 499 / 23768 | 86.48 / 87.28 | 262.1 / 259.7 | 93.65 / 94.78 | 9 / 73,488 |
| dynamic | 453 / 23768 | 93.14 / 87.52 | 243.4 / 259.0 | 100.26 / 90.93 | 9 / 73,488 |
| optional | 476 / 23768 | 92.25 / 89.59 | 245.7 / 253.0 | 89.34 / 91.54 | 9 / 73,488 |
| bridge | 5739 / 25527 | 207.21 / 170.62 | 117.5 / 142.7 | 226.52 / 159.51 | 11 / 98,256 |
| bootstrap | 2824 / 6534 | 78.32 / 48.77 | 79.6 / 127.8 | 73.57 / 58.28 | 9 / 24,528 |
| application | 22974 / 78508 | 827.14 / 568.52 | 90.5 / 131.7 | 809.29 / 587.63 | 12 / 196,560 |
| near-stored | 4194651 / 4194303 | 12525.22 / 11135.83 | 319.4 / 359.2 | 12826.86 / 11119.19 | 7 / 8,323,072 |
| near-fixed | 36667 / 4194303 | 16501.30 / 14886.27 | 242.4 / 268.7 | 17698.25 / 14966.84 | 17 / 11,009,856 |
| near-dynamic | 14330 / 4194303 | 14807.34 / 15056.99 | 270.1 / 265.7 | 17463.25 / 14797.25 | 17 / 11,009,856 |
| near-incompressible | 4195601 / 4194303 | 11036.32 / 11085.55 | 362.4 / 360.8 | 11213.95 / 11048.08 | 9 / 8,378,352 |
| near-huffman | 2132022 / 4194303 | 48956.38 / 30807.26 | 81.7 / 129.8 | 47336.47 / 31237.99 | 18 / 12,582,864 |

The application improves by about 31% and the Huffman-only case by 37% in these runs.
Small fixed decoding is within 1%; near-budget dynamic decoding is about 2% slower.
Stored-block copying and CRC are unchanged, so their variable timing is not evidence of a
new bulk-copy optimization. These observations support the small lookup for application
code and literal-heavy inputs, without claiming universal improvement.

Allocation counts and bytes are **identical before and after**. They count scalar `operator new`
requests during the loop, including string growth, rather than allocator bookkeeping or peak RSS.
Input, reference output and pre-reservation allocations are excluded. The decoder has no aligned
or array allocation path. Every `reserved` case makes **zero allocations** during decoding.
The fresh case reports cumulative allocated bytes, not simultaneously live or retained memory.

## Algorithm choice

A byte-bounded 64-bit reservoir replaces per-bit input indexing. It holds at most 39 bits for
reads of up to 32 bits, checks each byte before fetching it and tracks logical offsets separately
from prefetched bytes. Alignment discards only the fractional byte; bulk reads return complete
prefetched bytes to the input view. This preserves header CRC ranges, stored blocks, member
trailers, concatenation and strict trailing-byte rejection without reading past the supplied span.

A buffered-reader-only prototype gave a modest gain on the compiler bundle. The 128-entry
Huffman prototype reduced its fresh decode from approximately 743 to 613 µs; 256 entries reduced
it to approximately 534 µs in the exploratory run. The 256-entry table is retained: each tree
adds 512 stack bytes and no heap allocations. Codes longer than eight bits and short final input
use the validated canonical decoder. No large secondary tables or new tree acceptance rules are
introduced. Repeating-match and stored workloads remain dominated by copying and CRC validation.

## Reproduction

Configure the ordinary CEF-off Release build, then build the optional benchmark (not in CTest):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --target gzip_decoder_benchmark --parallel 2
python3 test/fuzz/GenerateGzipCorpus.py build/gzip-corpus path/to/build-distribution/compiler.mjs
build/test/unit/gzip_decoder_benchmark build/gzip-corpus/*.gz
```

The corpus directory must not already exist. Omit the optional bundle argument to use only
repository sources and generated neutral fixtures. The generator prints compressed/decoded
sizes and SHA-256 checksums. The benchmark also always runs the three small checked-in fixtures.
CSV reports time, decoded MiB/s, allocations and requested bytes per decode, for both output modes;
2,000 iterations per small input and 40 per input above 1 MiB. No timing assertion runs in CI.

For an exact historical comparison, copy only the two standalone headers into a build directory
and compile the same harness with that directory first in the include path:

```bash
mkdir -p build/gzip-baseline/webfront/http
git show d27e4fc:include/webfront/http/Gzip.hpp > build/gzip-baseline/webfront/http/Gzip.hpp
git show d27e4fc:include/webfront/http/BuffersPolicy.hpp > build/gzip-baseline/webfront/http/BuffersPolicy.hpp
clang++ -std=c++23 -O3 -DNDEBUG -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE \
  -ftrivial-auto-var-init=pattern -Ibuild/gzip-baseline -Itest/unit \
  test/unit/GzipDecoderBenchmark.cpp -o build/gzip-before
clang++ -std=c++23 -O3 -DNDEBUG -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE \
  -ftrivial-auto-var-init=pattern -Iinclude test/unit/GzipDecoderBenchmark.cpp -o build/gzip-after
build/gzip-before build/gzip-corpus/*.gz
build/gzip-after build/gzip-corpus/*.gz
```

Use the same compiler for both; adapt the standard-library hardening macro on other platforms.
Repeat three times, alternating before/after order. Desktop background activity makes these
local observations noisy; compare the corpus as a whole rather than treating a small timing
difference as a guaranteed gain.

## Differential and sanitizer validation

`WEBFRONT_GZIP_DIFFERENTIAL=ON` opts into test-only zlib linkage. Production remains header-only
and does not depend on zlib. `GzipZlibDifferential` checks the neutral fixtures in ordinary CTest;
`gzip_differential <corpus-directory>` also compares generated stored/fixed/dynamic/Huffman-only,
flush boundaries and size-boundary cases, exact budgets, sampled truncations/mutations, optional
headers, concatenation, CRC and trailing bytes. Comparisons require matching acceptance and
byte-for-byte content. The oracle explicitly loops over members and requires full consumption.

Intentional differences from permissive gzip consumers: WebFront rejects all trailing padding,
including zeros, and dynamic declarations of 31/32 distance symbols. The zlib oracle normalizes
strict consumption; arbitrary-input differential fuzzing compares every WebFront-accepted stream
with zlib, while the generated supported corpus checks acceptance in both directions. Reserved
or otherwise unsupported declarations are covered by WebFront's malformed-input unit fixtures.

```bash
cmake -S . -B build -DWEBFRONT_GZIP_DIFFERENTIAL=ON
cmake --build build --target gzip_differential --parallel 2
build/test/unit/gzip_differential build/gzip-corpus
ctest --test-dir build --output-on-failure --parallel 4
```

For Clang/compiler-rt sanitizer fuzzing, configure with `ENABLE_FUZZING=ON` and
`WEBFRONT_GZIP_DIFFERENTIAL=ON`. See [fuzzing](../test/fuzz/README.md) for commands.
The extended CI ASan/UBSan job runs the generated corpus and both gzip fuzzers; CodeScene
quality gates remain unchanged. The generated-corpus comparison step has a five-minute timeout.

On this macOS host, Homebrew's ASan runtime stalled in its allocator initialization before main.
Validation instead used Apple Clang 21.0.0's ASan/UBSan runtime; libFuzzer came from Homebrew LLVM
21.1.8 because Apple does not ship its archive. Both fuzzers ran for 30 seconds with a 1 MiB output
cap, 65,536-byte maximum mutation size and a five-second per-input timeout. Standalone differential
validation used the 4 MiB output limit with the complete generated corpus.

Local validation: 293 CEF-off CTest cases passed; 12,571 generated/fixture comparisons passed
under ASan/UBSan; bounded fuzzing completed 53,370 runs and differential fuzzing 44,065 runs
without a finding. The 264 bit-width/alignment combinations are covered by the unit tests.
