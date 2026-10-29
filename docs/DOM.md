# DOM from C++

`ui.document()` returns a `webfront::dom::Document<UI>` wrapper for the shared document
of one WebSocket connection. Copying the wrapper shares its name dictionary, pending
batch and node identities. A reload creates a new connection and document. A `Node`
is an identifier, not a local DOM object; copying or destroying it does not remove
anything in the browser.

The embedded fallback page installs the interpreter and lends an empty island named
`root`. An application can therefore build an interface without UI files:

```cpp
app.onUIStarted([](auto ui) {
    auto doc = ui.document();
    auto root = doc.island(); // default: "root"
    auto button = doc.element("button");
    auto label = doc.text("Click me");
    button.append(label);
    button.listen("click", [label](std::string) { label.setText("Clicked"); });
    root.append(button);
});
```

The optional [`@webfront/react` adapter](REACT.md) lends empty React `Island` components
and provides coalesced C++ stores; it is installed separately with `wf`.

A custom page lends only empty, disjoint elements that its UI library does not
reconcile. It opts into the document callbacks explicitly:

```js
import { installDomInterpreter } from '/webfront-dom.mjs';
const dom = installDomInterpreter(webFront);
const release = dom.island('panel', document.getElementById('panel'));
await dom.connect();
// Coordinate with C++ as described below before taking back the element or unmounting:
release();
```

`connect()` completes only after the DOM handshake and all queued revocations are
acknowledged. A failed attempt can be retried: after a partially successful attempt,
the acknowledged handshake and revocations are not resent; only unacknowledged
revocations remain pending. Releases made between attempts are queued too.
The server treats `wfDom.ready` idempotently on a live connection, so retrying a
readiness acknowledgement does not reset names or handles. This does not reconnect
a closed bridge. Await successful `connect()` before rebuilding C++ handles for the
page's new islands.

C++ uses `doc.island("panel")`. It cannot select arbitrary page elements, move or
remove island roots, or construct nodes from HTML. The page must call `release()`
before reclaiming a subtree; raw page mutations are not observed. Release invalidates
the root and descendants when its notification reaches C++. Remounting uses fresh
identifiers. `setText` accepts only a text node, preserving child identities.
`remove()` invalidates the entire removed subtree immediately in C++.
Attributes, properties and subscriptions require a C++-owned element: using them
on text nodes or island roots throws `std::invalid_argument` before changing the
pending batch or interning a name. Page-specific allowlist checks remain asynchronous.
`insertBefore(child, child)` is a no-op, matching the native DOM.

## Execution and batching

Create and use documents and nodes on the application's server context: in a C++
bridge callback, `onUIStarted`, or a task started by `app.spawn`/`ui.spawn`. Other
threads must submit a task; off-thread access throws `std::logic_error`. Shutdown
runs after the external run callers have returned, as in [ADR-006](adr/ADR-006-taches-structurees-et-arret.md).
The connection records its server thread when accepted. `ui.document()` checks
that thread before accessing the document table, including on the first call.
Creating a document in a close-notification callback throws `ConnectionError`; a
closing connection cannot acquire a new document.

The DOM API requires one execution thread for the application's context for the
entire connection. External callers of `run()`/`runOne()` must use that same thread;
do not run the context from several threads or alternate its execution between
threads while using DOM documents. `Node::valid()` and `Document::valid()` are
`noexcept` predicates for usability on the calling thread: both return false off
that thread, or for an invalidated handle. They do not make other DOM operations
safe to call from another thread.

Writes accumulate in one bounded batch. Once the interpreter calls `connect()`, the
first pending write schedules an owned task that waits **1 ms** on the server context,
then flushes the pending batch. This is a scheduling cadence, not a rendering deadline.
`doc.flush()` sends sooner; reads flush preceding writes before creating their sender.
Writes made before browser readiness remain queued. Shutdown discards pending writes
and drains the scheduled tasks; it does not promise a final browser update.

There is no implicit splitting: exceeding 1 MiB or 131,072 operations throws
`std::length_error`, leaving that failed operation unapplied to the pending batch.
Names are interned once per connection (65,536 entries, 1,024 UTF-8 bytes per name,
1 MiB total); there are at most 65,536 live nodes. Interned names are never reclaimed,
including names of removed or revoked islands. Repeatedly generating unique island
names permanently consumes this connection's dictionary budget. Reuse stable names;
a new connection/reload resets the dictionary. Identifiers are never reused in
a document. Flush earlier to bound a large construction. The browser validates the
whole batch before application, using the allowlist in [ADR-003](adr/ADR-003-protocole-binaire-et-bornes.md).
An invalid operation can still fail at the browser: C++ construction is not an
acknowledgement of successful DOM application.

## Events and reads

`node.listen(name, callback, {.preventDefault = true, .stopPropagation = true})`
declares browser-side flags at subscription time. An empty callback throws
`std::invalid_argument` without changing names, subscriptions or queued writes.
The callback receives a string:
input/change value (checkboxes: `"true"`/`"false"`), keyboard key, otherwise empty.
The callback runs on the server context and may queue writes or remove itself.
`unlisten(name)` and subtree removal release stored callbacks. Event values and
limits follow ADR-003. For bubbling input/change events the value belongs to the
subscribed node (`currentTarget`), not the originating child; it is empty if that
node has no `value` property. Flags cannot be decided after the asynchronous callback.

Event values above **65,535 UTF-8 bytes** cannot cross the bridge and are **not
delivered to C++**. They are not truncated. The interpreter logs the rejected
Promise to the browser console (`WebFront DOM event delivery failed`), including
encoding, transport and callback errors. There is no C++ event-error notification;
the document stays usable and an explicit grouped read can retrieve a larger value
within its separate 1 MiB result budget.

### Awaiting an event

Callbacks remain the usual way to handle events. For sequential dialogs and wizards,
where the coroutine is the state machine, `node.next(name, options)` returns a sender
of the next event's value, the string a `listen()` callback would receive:

```cpp
webfront::Task wizard(webfront::dom::Node forward, webfront::dom::Node back) {
    for (;;) {
        co_await forward.next("click"); // step 1 confirmed
        // ... show step 2 ...
        co_await back.next("click");
        // ... show step 1 again ...
    }
}
// In onUIStarted or a bridge callback: retain the handle to cancel the wizard.
auto handle = ui.spawn(wizard(forwardButton, backButton));
```

`next()` requires a C++-owned element, as `listen()` does, and throws
`std::invalid_argument` otherwise. Creating the sender subscribes nothing. Starting
it, on the server context, queues the browser subscription with the flags declared by
`next()`; `preventDefault` and `stopPropagation` therefore remain decided at
subscription time. The first matching event delivered to C++ completes the wait and
queues the unsubscription. Events are not queued while no wait is started, including
between two `co_await`s, and the subscription follows the batching cadence above, so an
event can be missed before it reaches the browser. Use `listen()` for a stream.

One subscription exists per element and event name: starting a wait while a
`listen()` callback or another wait holds that event completes with a
`std::logic_error`, and `listen()` throws `std::logic_error` while a wait is pending.
A wait completes stopped when it can no longer be satisfied: through the receiver's
stop token (`TaskHandle::requestStop()`, application shutdown), `unlisten()`, removal
or revocation of the node, a browser rejection that closes the document, and
disconnection or reload. A stopped coroutine unwinds without resuming after its
`co_await`. Starting off the server thread or on an invalid node completes with an
error. A stop requested from another thread completes on that thread, as for
`jsFunctionSender`; the server context later releases the obsolete subscription.
Event and stopped completions caused by the document run on the server thread.

### Grouped reads

Reads are explicit and grouped in one request. For example, in an owned coroutine:

```cpp
std::vector<webfront::dom::ReadRequest> requests;
requests.push_back({label, webfront::dom::Read::text});
requests.push_back({input, webfront::dom::Read::value});
auto sender = ui.document().read(std::move(requests));
auto values = co_await std::move(sender);
```

The result is a `std::vector<std::string>` in request order. Available reads are
`text` (`textContent`), `value`, `checked`, `width` and `height` (bounding rectangle,
CSS pixels as decimal strings). Missing value/checked properties return an empty
string. Layout reads require an element. There are 1..4,096 requests per group and
at most 1 MiB of encoded results. These are snapshots at browser execution time,
not continuously synchronized values. There are no implicit reads.

Calling `read()` flushes queued writes immediately, even if its returned sender is
never started or awaited. Only the grouped read request is lazy: the sender owns
its encoded request and dispatches that request at start. Cancellation completes
stopped locally and discards a late reply; browser execution continues. A revoked
node or closed document cannot return a successful late result. Disconnection and
browser read errors complete exceptionally. A successful result is decoded and
validated on the WebSocket receive thread, which is the sole server-context thread
under the DOM execution contract. Error/stopped completions may run on the thread
reporting disconnection or requesting stop, as for `jsFunctionSender`; they do not
execute the read's value-validation lambda. Owned application tasks resume on the
server context. See [ASYNC_EXECUTION.md](ASYNC_EXECUTION.md).

## Lifetime and failure

Disconnection, reload and application shutdown invalidate the old document and all
its nodes. Retained wrappers and nodes remain safe to inspect (`valid()` is false)
and mutations throw, including after the application is destroyed. This does not
extend the lifetime of the application's UI or application object.

`doc.onError([](std::uint32_t batchId, std::string reason) { ... })` receives browser
rejections, asynchronous send errors and exhausted batch identifiers. A rejection closes the C++ document because
its optimistic identities may differ from the browser. Reload to recover. Revoking
an island while any writes are pending also closes the entire document, including
unrelated islands. This can happen during an ordinary component unmount within
the 1 ms batching interval. Coordinate every page-initiated unmount as described
below when the document must remain usable. A callback may throw; its
bridge Promise rejects and browser-side event/revocation notifications handle that
rejection. The underlying tasks remain owned and drained by the application.

### Coordinating page unmounts

The application must implement a handoff between the page and C++ before removing
an island. Pause all writers for the document (including tasks and event callbacks,
even those updating other islands), flush, and await an explicit browser round trip
such as a grouped read before allowing the page to call `release()` and remove the
element. Pending writes only cover the pre-flush batch; a batch already sent but not
yet applied can still be rejected by the browser after release, closing the document
through `wfDom.rejected`. The browser round trip is needed for that in-flight case too. Keep writers paused until C++ has observed the revocation; use an
application-level acknowledgement before resuming writes or creating new handles.
`release()` itself returns no acknowledgement. A failed notification after connection
is logged to the browser console (`WebFront DOM island revocation failed`); it is not
automatically retried, so the application must coordinate recovery/reload before
assuming C++ has invalidated the island. `flush()` only queues a message,
and waiting 1 ms is not a synchronization barrier. An uncoordinated unmount may
therefore require reloading the entire document, not just recreating that island.

The document API reserves `wfDom.ready`, `wfDom.revoked`, `wfDom.event`,
`wfDom.rejected` and the browser function `wfDom.read`. After `dom.connect()`, do not mix low-level
`sendDomBatch` commands with `Document` on the same connection: the document owns
its name and node tables. Low-level interpreter users that do not call `connect()`
retain their own callbacks, including after obtaining a dormant C++ document wrapper.
Custom network providers need `ResumeAfter` to use this
optional document API; an unsupported `wfDom.ready` rejects explicitly with
`DOM API requires Net::ResumeAfter`. The interpreter accepts trusted application commands, not
untrusted content; see [EMBEDDED_ASSETS.md](EMBEDDED_ASSETS.md).

The fallback first awaits `webFront.ready`, then imports and connects the DOM
module inside its error handler. The import is relative to the fallback module,
matching its relative script resources when served below a URL prefix. A missing module or unsupported DOM API is shown
as `Connected to WebFront; DOM unavailable: ...` (`data-state="dom-error"`), distinct
from an initial bridge connection failure.
