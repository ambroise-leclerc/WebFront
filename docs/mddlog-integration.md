# Optional mddlog diagnostics

`WEBFRONT_USE_MDDLOG` defaults to `OFF`. In that configuration WebFront remains
header-only and builds with CMake 3.31, its usual compilers and generators. It neither
searches for mddlog nor selects CMake's experimental `import std` gate.

With `ON`, WebFront owns `include/tooling/MddlogLogger.hpp`, the shared declarations
in `include/tooling/LoggerApi.hpp`, and `logging/Logger.cpp`. `tooling/Logger.hpp`
selects the facade through the `WEBFRONT_USE_MDDLOG=1` definition supplied by the
`WebFront` target. The `WebFront_mddlog` static library (`WebFront::mddlog`) compiles
the sole module-consuming translation unit. Linking `WebFront` supplies the definition
and adapter automatically. All translation units in an application must use the same
option; do not mix the two logger implementations or define the macro independently.
Consumers include headers and do not import modules. mddlog remains a module library;
WebFront's facade is neither a header-only implementation nor an installed mddlog API.

## Dependency and build choices

Use an installed package with `find_package(mddlog CONFIG REQUIRED)` (the default
enabled path), or set `WEBFRONT_MDDLOG_SOURCE_DIR` to a source checkout. The latter
builds only its module libraries in a separate binary directory. mddlog tests/examples
are disabled in this dependency scope, without changing the caller's cache options.
The adapter privately links both `mddlog::mddlog` and `mddlog::core` because it imports
core modules directly. The final executable receives the static dependency closure.

For example, with an upstream Clang/libc++ toolchain providing its std module manifest:

```bash
cmake -S . -B build-mddlog -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/mddlog/cmake/toolchains/linux-clang21-libcxx.cmake \
  -DWEBFRONT_USE_MDDLOG=ON -DWEBFRONT_MDDLOG_SOURCE_DIR=/path/to/mddlog
cmake --build build-mddlog --parallel
ctest --test-dir build-mddlog --output-on-failure --no-tests=error
```

For the installed path omit `WEBFRONT_MDDLOG_SOURCE_DIR` and pass
`-DCMAKE_PREFIX_PATH=/path/to/mddlog-install`. Both paths need the same compiler,
standard library, std module manifest, architecture, configuration and MSVC runtime
as mddlog. The enabled path preserves the toolchain's MSVC runtime selection (normally
`/MD`, `/MDd`); the standalone path retains WebFront's historical static runtime.
On POSIX the enabled path uses `Threads::Threads` rather than adding directory-wide
`-pthread` after CMake has created the std module target: mismatched thread flags can
make Clang reject that BMI. If a toolchain requires explicit thread flags, supply them
in its initial C++ flags so std and every importing translation unit agree.

## Qualified tools

The constraints below mirror mddlog's `CMakeLists.txt` at
`5b43265664d83c3b7135d328f611e685792a1b02`; its support matrix is tracked in
[issue #5](https://github.com/ambroise-leclerc/mddlog/issues/5).
Admission floors do not mean every newer release has been verified.

| Platform | Compiler | CMake and generator |
|---|---|---|
| Linux | GCC >=16.1, excluding 16.2; CI pins 16.1.0 because 16.2 corrupts BMIs | 4.0–4.3; Ninja family |
| Linux | upstream Clang >=20 with std module metadata; reference CI uses Clang 21/libc++ | 4.0–4.3; Ninja family |
| Windows | MSVC compiler version >=19.40; reference CI uses Visual Studio 2022 | 4.0–4.3; Ninja family |
| macOS, Apple Silicon only | upstream LLVM/Clang **21.1.8 exactly**, libc++, llvm-ar/llvm-ranlib 21.1.8 | **4.3.1 exactly**; Ninja family |

CMake <4.0 and >=4.4 are rejected on the enabled path. The experimental UUID is
`d0edc3af-4c50-42ea-a356-e2862fe7a444` for 4.0–4.2 and
`451f2fe2-a8a2-47c3-bc32-94786d8fc91b` for 4.3, selected **before** `project()`.
AppleClang, Intel macOS, other Apple platforms, other operating systems, and
non-Ninja generators are rejected. Compiler/stdlib C++23 `import std` metadata is
required (MSVC retains mddlog's >=19.40 fallback). Configuration, compilation,
linking and test execution are distinct acceptance stages.

## API and ownership

The original logger call syntax is retained. `Disabled` clears all severity groups;
Debug/Info/Warn/Error map explicitly. Disabled calls skip formatting. Debug captures
the caller's location even on LLVM; `infoHex` emits its text and dump separately,
with ASCII limited to bytes 0x20–0x7E regardless of char signedness.
A single `addSinks` callback returns an opaque `SinkHandle`; several callbacks return
an array of independent handles. `removeSinks` requires those handles and waits for
quiescence outside callbacks. Handles are explicit registrations, not RAII subscriptions.
Remove registrations before destroying their captured objects. `WebLink` stores the
selected backend's registration type instead of assuming an integer index.

The adopted facade includes #70's thread-local context/writer binding and observable
refusals. `infoHex()` returns independent `message` and `dump` outcomes; a refused
record's own truncation flag stays false. `lastWriteOutcome()` instead summarizes the
pair: status/field select the first refusal (or the dump when both are written), and
its truncation flag is true if either admitted record was truncated. A summary can
therefore report RingFull and truncation together without marking the refused record.
Removing that aggregate flag would lose truncation from an admitted message when its
dump is refused or is short enough to remain complete. A host must copy borrowed record fields synchronously and remove writer
bindings before destroying its producer ring, including exceptional exits. Legacy text
delivery stays available. For the diagnostic/audit boundary, follow
[mddlog's facade contract](https://github.com/ambroise-leclerc/mddlog/blob/develop/docs/migration/webfront-facade.md).
ADR-003 remains **Proposed**; accepting it is a separate review decision.

## Browser transport lane (mddlog#72)

With the option on, the browser log sink no longer runs on producer threads. `addTransport`
registers it on mddlog's bounded `TransportConsumer`; the first registration starts one
WebFront-owned consumer thread. Producers write only their own SPSC ring and never wait
for a transport. `transportProducerRings` (32) rings of `transportRingCapacity` (128)
records are registered before that thread starts. A thread claims a free ring on its first
emission while a transport is attached, and returns it when the thread exits; the pool
lock orders each hand-over, so a ring never has two producers. With every ring claimed,
emission returns `RingUnavailable`; a full ring returns `RingFull`. Neither waits.

The consumer thread logs into its own consumer ring. A record emitted there while a
transport is dispatching (for example a write error reported synchronously by the socket)
is acknowledged without delivery to any transport and counted in `reentrantRecords`;
deferring it to the next drain would only postpone the feedback loop. A transport
receives a `TransportRecord`: the context captured at emission (component, `operationId`,
`correlationId`), level, time, location, governed message (160 bytes, truncation flagged)
and its legacy rendering. Views end with the callback. `transportHealth()` reports
delivered records, synchronous and reported failures, detachments, reentrant records,
ring refusals, unavailable rings, abandoned drains and active transports without using
any sink. `flushTransports()` waits for a complete drain started after the call; it
returns false on the consumer thread.

Producers skip the rings while no transport is attached. Records refused by the rings
still reach synchronous text sinks, as `RingFull` from a host writer already did.
The facade, its transports and `TransportRecord` carry diagnostics only; no audit event
type is accepted, and mddlog checks at compile time that `TransportConsumer` accepts
neither an audit ring nor an audit sink.

Without the option, the same calls map to the historical synchronous sink, and
`TransportRecord` carries only the rendered text. `ContextScope` is then a no-op.

### WebLink lifecycle

- The handshake attaches one transport sending `debugLog` commands; its handle is stored
  and is never an RAII subscription.
- The rings bound producers, but a browser that stops reading would otherwise let the consumer
  move every record into the WebSocket's write queue. Diagnostic frames therefore go through
  `WebSocket::tryWrite`, refused once `WebLink::maxPendingLogFrames` (64) frames of any kind
  await the network. A refused frame is dropped without logging, counted by
  `WebLink::droppedLogFrames()` and in `TransportHealth::overflows`. Application frames
  (calls, returns, script injection) keep the unbounded queue and are never dropped.
- A WebSocket write error invokes `onWriteError` **before** its diagnostic, even after
  `stop()`, because frames queued earlier still fail. WebLink then calls
  `reportTransportFailure`, retiring the transport before anything logs the failure.
  When the failing write completes synchronously inside the transport, the removal is the
  registry's deferred self-removal.
- A normal close calls `removeTransport`. The reference emits no
  `WebLinkEvent::Code::closed`; a link stays owned by `BasicWF` until destroyed, and its
  destructor removes the transport, waiting for any in-flight write, before logging.
- Detachment happens at most once per link and no transport attaches afterwards.
- The WebSocket can outlive its link while asynchronous operations complete. Its message,
  write-error and close handlers share a `HandlerGuard` with the link: the destructor first
  stops new handler invocations and waits for those running on other threads, so a late
  completion never reaches the destroyed link or a successor allocated at its address.
- WebLink installs `ContextScope` at its entry points: `weblink` for received messages,
  close and destruction; `cppFunction` with `js-cpp:<CallId>` for calls from JavaScript;
  `jsFunction` with `cpp-js:<CallId>` for returns of C++-initiated calls.

`main` in `HelloWorld.cpp` and `JasmineTest.cpp` now keeps the console handle and removes
it before returning. `LoggerTests.cpp` already stored and removed its handle and remains
unchanged.

## Verification

`LoggerTests.cpp` is unchanged. Enabled builds additionally compile the module-free
`MddlogLoggerTests.cpp` for masks, LLVM caller locations, independent registrations,
every hex byte, nested context, structured refusal and independent legacy delivery.
`MddlogTransportTests.cpp` covers the seven events of ADR-003 Decision 6 through two
transports after their context strings are destroyed, legacy rendering and truncation,
saturation while the consumer is blocked, synchronous throws, reentrant logging,
asynchronous failure reports, removal during concurrent emission, and WebLink
disconnection, synchronous and asynchronous write failures and destruction before
the drain. `WebLinkTests.cpp`, built in both configurations, reproduces a browser
that stops reading: 10,240 diagnostics with one write never completing leave 64 frames
queued and 10,176 counted refusals. The `sanitizers` job of `MddlogIntegration.yml` runs these scenarios under
AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer on Linux/Clang 21,
instrumenting the whole build, including mddlog and the std module, through
`CMAKE_CXX_FLAGS` and `CMAKE_EXE_LINKER_FLAGS`. LLVM 21.1.8's sanitizer runtimes do not
start on macOS 27 (an empty program hangs under ASan and crashes under TSan), so local
macOS runs cannot provide this evidence.
The normal CI uses CMake 3.31.10 with the option off. `MddlogIntegration.yml` builds
and tests both source and installed integration on Linux/Clang. The accompanying
mddlog integration tests exercise these builds on its full supported CI matrix.

`MddlogHeaderConsumer.cpp` includes only `tooling/Logger.hpp` and links only
`WebFront::mddlog`, checking the facade target's complete usage requirements and execution.
