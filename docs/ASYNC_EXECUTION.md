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
Timeout composition remains future work.

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
