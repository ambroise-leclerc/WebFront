# Decoder fuzzing

`ENABLE_FUZZING=ON` builds `fuzz_messages`, `fuzz_http`, `fuzz_websocket`, and `fuzz_gzip` with
Clang/libFuzzer, ASan, UBSan, standard library hardening and pattern initialization.
Linux with Clang/compiler-rt is validated in CI. Apple Clang does not include libFuzzer;
on macOS install it externally (for example as part of LLVM/compiler-rt) and use compatible
ASan/UBSan runtimes. CMake rejects missing runtimes during configuration.
It works independently of `ENABLE_TESTING`; no Catch2 or CEF is needed.

The binary corpus files use the existing wire formats. Seeds derive from
`MessagesTests.cpp` (handshake, print call, scalar, tuple, typed arrays and return),
`HTTPServerTests.cpp` and `AcceptEncodingTests.cpp` (GET, upgrade and encoding preferences), and `WebSocketTests.cpp`
(masked Hello, empty binary frame, extended length and an oversized frame).
Empty strings, integer overflow and malformed HTTP versions retain hardening
regressions. Message seeds use native little endian, as on the CI hosts.

CMake copies these files to `build-test/test/fuzz/corpus/<decoder>` at configuration.
Run against that copy, since libFuzzer adds and minimizes inputs in its corpus:

```sh
./build-test/test/fuzz/fuzz_messages build-test/test/fuzz/corpus/messages \
  -max_total_time=30 -timeout=5 -max_len=65536 -rss_limit_mb=1024
```

Repeat for `http`, `websocket` and `gzip`. Gzip seeds cover stored, fixed/dynamic Huffman
blocks, optional header fields, empty members, corruption and truncation. The gzip harness
caps total decoded output at 1 MiB; the HTTP server defaults to 4 MiB. Input errors caught by the harness are limited
to the decoder's documented rejection exceptions; allocation failures, sanitizer
findings and hardening assertion failures must stop the process. WebSocket
payload budgets exercise rejection before allocation of attacker-supplied lengths.
CI retains reproducer artifacts on failure. Reproduce a finding by passing its
file to the same executable, then add a unit regression before fixing it.


## Gzip/zlib differential checks

Opt in with `-DWEBFRONT_GZIP_DIFFERENTIAL=ON`: tests link zlib, production headers do not.
With `ENABLE_TESTING=ON`, `gzip_differential` compares the neutral fixtures in CTest;
with `ENABLE_FUZZING=ON`, `fuzz_gzip_differential` additionally checks arbitrary accepted
streams against zlib under ASan/UBSan:

```sh
python3 test/fuzz/GenerateGzipCorpus.py build-test/gzip-corpus
build-test/test/unit/gzip_differential build-test/gzip-corpus
build-test/test/fuzz/fuzz_gzip_differential build-test/test/fuzz/corpus/gzip \
  -max_total_time=30 -timeout=5 -max_len=65536 -rss_limit_mb=1024
```

The corpus directory must be new. An optional second generator argument supplies an application
bundle to compress. Generated supported streams check acceptance and content in both directions;
the fuzz target checks every WebFront-accepted stream, allowing intentional rejections of
unsupported declarations. The oracle handles concatenated members and requires exact consumption,
normalizing zlib consumers' permissive treatment of trailing padding. See
[HTTP asset validation](../../docs/HTTP_ASSETS.md#manual-benchmarks-and-differential-validation) for sizes, limits and reproduction.

On macOS with Apple Clang and an external libFuzzer archive, the standalone equivalent is:

```sh
clang++ -std=c++23 -O1 -g -fsanitize=fuzzer-no-link,address,undefined \
  -fno-sanitize-recover=all -Iinclude test/fuzz/GzipDifferentialFuzz.cpp \
  /path/to/libclang_rt.fuzzer_osx.a -lz -o build-test/fuzz_gzip_differential
```

Use `GzipFuzz.cpp` in the same command without `-lz` for the bounded non-differential harness.
This allows the compiler and its ASan runtime to remain paired when a different toolchain supplies
libFuzzer. A standalone differential runner uses `GzipDifferential.cpp` with
`-fsanitize=address,undefined -lz` and no libFuzzer archive.
