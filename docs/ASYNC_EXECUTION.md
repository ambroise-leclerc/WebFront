# Structured execution and browser calls

WebFront uses the C++26 sender vocabulary through `networking::ex = ::stdnet::ex`.
The current stdnet dependency selects stdexec; switching that implementation to
`std::execution` remains stdnet's responsibility. Network operations still come
from stdnet, rather than a standard networking library.

The implementation uses P2300 algorithms and environments, `task`, and the
[P3149 structured scope facilities](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3149r11.html):
`counting_scope`, `spawn`, and `join`. Sender implementation headers enter only
through `networking/SenderHeaders.hpp`, which protects them from CEF macros.

## Network lifetime

The HTTP server owns one `networking::TaskScope` for the accept loop, HTTP
connections and upgraded WebSocket read/write loops. Each WebSocket also has its
own stop source, linked to the server's stop source. Closing one connection
therefore does not cancel other connections. The scope belongs to the server,
rather than a WebSocket: the final operation can release the socket before
releasing its scope association.

`stop()` can be called from another thread. It requests cancellation and stops
the context. After the thread running the context has returned, the owner calls
`drain()`: close the scope, request stop, wait for in-flight `spawn()` calls to
finish starting their senders, restart the context, and pump one
completion at a time until `join()` completes. Socket cleanup still runs on the
context thread. Joining tracks the complete operation lifetime, including its
destruction, and does not require unrelated context work to finish. If `run_one()`
reports no work while tasks remain, `drain()` throws `std::logic_error` instead
of spinning. The startup wait prevents a concurrent scope association from
being mistaken for a stalled task before its sender has started or queued work.
Sender startup and stop callbacks run without holding the spawn admission mutex,
so cleanup can attempt another spawn and receive a rejection.
It retains the join operation, allowing a low-level owner to
complete the external operation and retry. The scope still must be joined
before destruction. Application tasks must cooperate with cancellation;
shutdown cannot safely abandon a suspended coroutine and its captures. The destructor
uses a nonthrowing join path: it logs a stalled join once and retries with a short
sleep until the operations finish, keeping their owners alive. It can therefore
wait indefinitely for a non-cooperative operation, but does not spin or propagate
a drain exception. Explicit `shutdown()` keeps reporting the error so its caller
can complete the external operation and retry. An external completion in flight
can also make the explicit no-work check report a pending join.

Low-level users constructing `WebSocket::create` or `WebLink` directly now pass
an external `TaskScope&`. That scope and the context must outlive their tasks;
stop/destroy the links and drain the scope before destroying their dependencies.
`BasicWF` does this for application users. An internal scope owned by a
WebSocket is deliberately avoided: its last task could destroy it before
releasing the scope association.

## Awaitable calls

The accepted API adds an explicitly typed sender form alongside the existing
future form:

```cpp
auto result = co_await ui.jsFunctionSender<double>("Math.sqrt")(81);
```

`webfront::Task` is the public coroutine type. `app.spawn(task)` or
`ui.spawn(task)` starts it on the server context, owns it until completion and
cancels it during application shutdown. No scheduler, scope or context is
needed. For example:

```cpp
webfront::Task showSquareRoot(webfront::UI ui) {
    auto result = co_await ui.jsFunctionSender<double>("Math.sqrt")(81);
    co_await ui.jsFunctionSender("console.log")("sqrt(81) =", result);
}
```

```cpp
auto handle = app.spawn(showSquareRoot(ui));
// Later: cancel this task, leaving other tasks and the connection active.
handle.requestStop();
```

`ui.writesAtMost(limit)` returns a sender completing once at most `limit` frames
wait for the network on that browser's connection, woken by the write that brings
them down (no polling), or at once when the browser is gone. A closing connection
or a released link also completes it, after the closure was reported; it observes
the task's stop token. It paces output to a browser that reads slower than written.

`app.resumeAfter(duration)` and `ui.resumeAfter(duration)` return a sender that
completes on the server context once the duration has elapsed. Awaited in a task,
it observes the task's stop token: `TaskHandle::requestStop()` or shutdown
completes it stopped at once, so a waiting coroutine never delays shutdown.
`app.subscribeUIClosed(handler)` calls the handler on the server thread when a
browser closes its connection or the connection fails (a failed write included),
for as long as the returned `UISubscription` lives; shutdown, which stops every
connection itself, notifies no handler. The connection is then released by a task
on the server context; a call made meanwhile on another thread keeps the link alive
until it returns (`findLink` shares it). Afterwards, an untyped `jsFunction` call to that UI is
dropped, a typed call returns a future that throws `Browser connection closed`,
a `jsFunctionSender` completes with an error when started, and `addScript` throws
`ConnectionError`. The 16-bit identifier of a released connection is reused only
after 65,536 later connections.

A `UI` is an assignable value: it can be kept in a `std::vector` (removed with
`std::erase_if`) or a `std::optional`, and assigned. Two values are equal when they
belong to the same application and have the same `id()`. A value identifies its
connection only until that connection closes: once the identifier is reused, a value
kept after the close equals the later browser's and reaches that browser (#428 tracks
rejecting such stale values). Remove kept values in the `subscribeUIClosed` handler.
A value must not outlive its application, and assigning one is not atomic: a
container of values shared between threads needs a lock.

`app.spawn(task)` and `ui.spawn(task)` may be called from any thread while the
application object remains alive, including concurrently with stop/shutdown.
Coroutine bodies and their resumptions execute on the server context. Synchronize
object destruction with callers; pass owned arguments into coroutine functions.
`TaskHandle::requestStop()` is also callable from any thread.

Discarding a `TaskHandle` keeps its task running. A retained handle remains safe
after application destruction. `app.stop()` requests shutdown;
`app.shutdown()` also joins owned work before returning, and is called by the
destructor. A call rejected by stop/shutdown returns an already stopped handle without
starting the coroutine. A task accepted before shutdown can start and then be
cancelled. The stop callback is installed before scope association, and that
association decision determines acceptance. Call `shutdown()` outside server callbacks and after any external
callers of `run()`/`runOne()` have returned. Objects referenced by coroutine
parameters must outlive the task; pass values or owned captures when needed.
Use coroutine functions whose arguments own their data; a temporary capturing
coroutine lambda can leave its captures dangling after its closure is destroyed.

`jsFunctionSender(name)` defaults to `void`; use `jsFunctionSender<R>(name)`
when a return value is needed. Unhandled task errors are logged; ordinary
`try`/`catch` inside the coroutine handles browser failures. Cancellation
unwinds the coroutine rather than resuming beyond its `co_await`.

Calling the function object encodes and owns its arguments, including data from
string views and spans; encoding errors throw at that point. Connecting the
sender creates an operation. Starting it allocates a correlation ID and sends
the frame. A sender can be connected repeatedly; each operation is a separate
browser call. It holds weak connection references, so starting after the link
is destroyed reports an error without accessing the application. Creating the
function object still requires a live UI/application.

Completions are `set_value(R)` (or `set_value()` for void),
`set_error(std::exception_ptr)` for JavaScript errors, malformed returns or
disconnection, and `set_stopped()` for cancellation through the receiver's stop
token. `co_await` propagates a task's token. `TaskHandle::requestStop()` cancels
one application task; low-level users may still manage their own scope.
Completion runs on the
thread delivering the result, disconnection or stop request; callers must
synchronize shared application state. No scheduler hop is promised by this
sender itself. Application tasks spawned by WebFront resume on its server
context. Neither sender calls nor future calls have a delivery acknowledgement
for the write queue or an automatic response deadline. An unsuccessful write
that does not report closure can leave a call pending until disconnection;
sender waits can also be ended by task cancellation or application shutdown.
Timeout composition with `resumeAfter` remains future work.

Cancellation before start sends no frame. Cancellation after dispatch stops
waiting locally; JavaScript already running continues, and its late response
is discarded. No remote cancellation command is sent to the browser.
Cancellation removes the registry's reference to the receiver; the caller may
destroy the operation inside its completion. Only the cancelled
16-bit ID remains reserved until the response or disconnection, preventing a
late response from completing a different call after counter wrap. Discarding a registration also checks its
operation owner, so an obsolete completion cannot cancel a replacement using
the same wire ID. Enough
unanswered cancelled calls can exhaust IDs, producing an error until responses
arrive or the connection is replaced.

Result, disconnection and stop may race: exactly one terminal completion wins.
A receiver may destroy the operation inside that completion. Registering an
already-stopped token also completes safely, without dispatching.

`ui.jsFunction<R>(name)(...)` remains an eager `std::future<R>` call, with no
cancellation API. The untyped `ui.jsFunction(name)` remains fire-and-forget.
JavaScript-to-C++ calls still always return Promises. None of these changes adds
richer bridge result types or remote Promise cancellation.

## HTTP gzip CPU work

Cold gzip identity fallback is a sender backed by one lazy, owned worker per HTTP server.
The worker reads owned compressed bytes and fills preallocated output; it never calls filesystem
providers or manipulates network objects. Completion resumes on the connection's I/O scheduler,
where validation results enter the cache and HTTP headers/body are finalized. Bounded response
leases survive through the network send, including cache-hit responses and HEAD consumers.

Stop closes admission; queued work without live consumers is skipped. An active decode runs
to completion before cancellation is delivered locally. Peer disconnects are discovered at the
subsequent write. After external run callers return, server drain joins CPU work before pumping
and joining its existing task scope. No CPU task or encoded/decoded buffer is detached from its
owner. See [HTTP asset encoding](HTTP_ASSETS.md) for 503 backpressure, memory reservations and
remaining synchronous provider-read and non-interruptible-decode limits.
