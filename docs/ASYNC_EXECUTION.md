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

## Threads

WebFront runs its HTTP/WebSocket server on one thread: the thread `show()` starts, or the thread
that calls `run()` or `runOne()`. That server thread is the only one that runs:

- `cppFunction` callbacks, `onUIStarted`, `subscribeUIStarted` and `subscribeUIClosed` handlers;
- DOM event callbacks and the bodies of tasks started with `app.spawn` or `ui.spawn`.

They run one at a time, in the order of the events that cause them, and never in parallel with one
another. A long callback delays every browser, so do blocking work on another thread or in a task.

From any thread, while the application lives, you can call:

- `ui.jsFunction(name)(...)`, `ui.jsFunction<R>(name)(...)` and starting a `jsFunctionSender`;
- `ui.addScript`, `ui.pendingWrites`, `ui.writesAtMost` and `ui.spawn`;
- `app.broadcast(name)(...)`, `app.forEachUI`, `app.connectedBrowsers`, `app.spawn` and `app.stop`.

These calls never wait for the server thread. They take short internal locks, queue the frame,
and the server thread writes it. Only `ui.document()` and its nodes are confined to the server
thread. Register `cppFunction` callbacks and set `onUIStarted` before the server starts. You can
add or remove subscriptions at any time.

WebFront never holds one of its own locks while it calls application code. A thread can therefore
call WebFront while it holds an application lock that a `cppFunction` callback also takes. The
callback waits for that lock, and the call it waits for does not depend on the callback. What
deadlocks is waiting for the server thread itself:

- the `get()` of a `jsFunction<R>` future inside a callback (the server thread reads the reply);
- the same wait on another thread under a lock that a callback takes;
- `app.shutdown()` or the destruction of the application under such a lock.

Inside a task, await `jsFunctionSender<R>` instead.

## Broadcasting to every browser

`app.broadcast(name)` returns a function object that calls the JavaScript function `name` on every
browser whose `onUIStarted` and `subscribeUIStarted` handlers have returned and whose closure was
not reported yet. Like the untyped `jsFunction`, the call is fire-and-forget. The application keeps
no table of its browsers: those that connect later receive the next calls, and a browser that
closes stops receiving them. A call made while a browser's handlers run reaches it right after
them, so what the handlers send, such as the initial state, arrives first. Each call encodes its
arguments once, into a frame that all the browsers share.

```cpp
auto onState = app.broadcast("siliconForge.onState", {.maxPendingWrites = 2});
// On a thread of the application, about 30 times per second:
onState(stateJson);
```

`BroadcastOptions::maxPendingWrites` protects a browser that reads slower than the application
writes. A call is queued for a browser only while fewer calls of the same function than that await
its network. Otherwise the call is held back, and a newer call of that function replaces it. The
held call is queued as soon as the browser catches up, so the browser skips the intermediate
states but always receives the latest one. Calls of a function reach each browser in the order
they were made. Other frames, such as logs or other functions' calls, do not count toward the
limit and are never held back. Order across functions is therefore not kept: a held call reaches
the browser after the frames written meanwhile. If the browser must see a state before a later
call of another function, send both through the same function or without a limit. Calls of one
function made with different limits share the browser's queue, and each call applies its own
limit. While a browser's handlers run, at most `maxPendingWrites` calls of the function are kept
for it, the newest replacing the last one kept. Without the option, every call is queued for
every browser. The held calls belong to each browser's connection and go with it, so the
function object holds no state and may be a temporary.

`app.forEachUI(function)` calls `function(ui)` on the calling thread, outside every WebFront lock,
for per-browser arguments or typed calls. It visits the browsers whose handlers had returned when
it started: unlike a broadcast, it skips a browser whose handlers are still running. As anywhere
else, `ui.document()` throws `std::logic_error` outside the server thread.

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
