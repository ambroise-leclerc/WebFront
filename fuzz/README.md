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

CMake copies these files to `build-fuzz/fuzz/corpus/<decoder>` at configuration.
Run against that copy, since libFuzzer adds and minimizes inputs in its corpus:

```sh
./build-fuzz/fuzz/fuzz_messages build-fuzz/fuzz/corpus/messages \
  -max_total_time=30 -timeout=5 -max_len=65536 -rss_limit_mb=1024
```

Repeat for `http`, `websocket` and `gzip`. Gzip seeds cover stored, fixed/dynamic Huffman
blocks, optional header fields, empty members, corruption and truncation. The gzip harness
caps total decoded output at 1 MiB; the HTTP server uses 64 MiB. Input errors caught by the harness are limited
to the decoder's documented rejection exceptions; allocation failures, sanitizer
findings and hardening assertion failures must stop the process. WebSocket
payload budgets exercise rejection before allocation of attacker-supplied lengths.
CI retains reproducer artifacts on failure. Reproduce a finding by passing its
file to the same executable, then add a unit regression before fixing it.
