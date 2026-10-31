# React stores and C++ islands

`@webfront/react` is an optional package, installed by `wf` from an npm-compatible
registry. Its sources and declarations live in `js/react`; it is not an embedded
core resource. React remains a peer dependency, shared with the application. `@types/react` is an
optional peer for TypeScript projects; install it explicitly alongside TypeScript.
No `react-reconciler` is used. The peer ranges are React `^19.3.1` and optional
`@types/react` `^19.3.0`; CI tests React 19.3.1 and `@types/react` 19.3.0. `useSyncExternalStore` reads cached snapshots;
development React 19.3.1 is exercised under `StrictMode`. This lower bound is a
support/qualification policy based on that CI baseline, not a technical requirement
of the hooks: the hooks used by the adapter exist in React 18, but React 18 and
19.0–19.2 have not been qualified here.

## Store

Install the browser receiver before asking C++ to send:

```tsx
import {createStore, useStore} from '@webfront/react';
const statistics = createStore<[number, string]>('statistics', [0, 'Waiting']);
function Statistics() {
    const [count, label] = useStore(statistics);
    return <p>{count}: {label}</p>;
}
```

In a server-context callback, construct and retain a store for that browser:

```cpp
#include <webfront/components/react/Store.hpp>
using Snapshot = std::tuple<std::uint32_t, std::string>;
webfront::react::Store<UI, Snapshot> statistics{ui, "statistics"};
statistics.set({1, "Running"});
statistics.set({2, "Running"}); // replaces the pending snapshot
```

Copies share pending state. Moving a store transfers its wrapper: the source's
`valid()` returns false, and `set`, `flush`, `close` and `onError` throw
`std::runtime_error` on that moved-from wrapper. The first `set` schedules an application-owned task,
which waits 1 ms on the server context. Only the last complete snapshot is sent.
A burst of 1,000 `set` calls in one callback produces one bridge message and one
React commit in the browser regression. This is a server cadence, not a rendering
deadline: writes in later cycles can cause additional commits. Different stores
have independent cycles; fields requiring an atomic update belong in one tuple.
`flush()` queues the latest snapshot early, without awaiting the browser.

`T` must be a supported bridge value: scalar, string, tuple, numeric typed array
or packed bitset. Tuples arrive as arrays. Ordinary JavaScript objects do not cross
the bridge. Snapshot identity is stable until a message arrives; treat snapshots,
including nested arrays and typed arrays, as immutable. Subscribe functions stay
stable and return an unsubscribe function. An identical snapshot by `Object.is`
does not notify. Store names contain ASCII letters, digits, `_` or `-`. The browser namespace
`webfrontReactStores` is reserved for these receivers. A throwing subscriber is
logged to the browser console and does not interrupt the remaining subscribers;
the adapter contains diagnostic logging failures too.

The C++ store shares its connection's document lifetime and thread rules, even
when no island is used. It does not require `dom.connect()` to send data. Create
and mutate it on the server context. Disconnect, document rejection and application
shutdown invalidate it; the timer is owned and drained by the application.
`valid()` returns false off that thread. Retained stores are safe to inspect after
the application has been destroyed; they do not extend the lifetime of `UI`.
`close()` discards pending values, releases the installed error callback and
invalidates all copies. Synchronous errors
from `set`/`flush` throw. An explicit `flush()` encoding/send failure retains the
pending snapshot and leaves the store open for retry (including the scheduled
cycle). Only a successful send clears that pending snapshot. An automatic
encoding/send failure closes the store and calls `onError(std::exception_ptr)` if
installed. The automatic failure consumes and clears that callback before invoking
it, so a callback capturing a store copy does not keep a closed store alive.
Self-owning callbacks still require an explicit `close()` on the normal path;
prefer weak captures when that lifecycle cannot be guaranteed.
Exceptions thrown by `onError` are caught and logged; they cannot escape
the automatic-send coroutine. Browser function failures are
not acknowledged by this untyped, fire-and-forget C++ call.

Create only one C++ store per browser/name and share its copies; two independently
constructed stores do not coalesce together. Browser `createStore` rejects duplicate
names. `dispose()` removes the receiver and subscribers; coordinate it with C++
`close()` after stopping writers. Store disposal is explicit, independent of a
component's StrictMode cleanup. Unmount all consumers before disposal: subscribing
to a disposed store throws, including a later `useSyncExternalStore` resubscription
while rendering a mounted consumer. Optional `{target: window}` installs a receiver
in another window; the default is `globalThis`.

## Island

Load the optional server interpreter and pass it to an empty component:

```tsx
import {Island} from '@webfront/react';
async function prepareDom() {
    const interpreterUrl = '/webfront-dom.mjs';
    const {installDomInterpreter} = await import(interpreterUrl);
    return installDomInterpreter(webFront);
}
// After prepareDom() resolves, render inside your existing React tree:
// <Island dom={dom} name="panel" className="panel" />
```

`Island` renders a `div` without children. Its layout effect lends that element;
after the React commit, `await dom.connect()` and ask a C++ callback to build
`ui.document().island("panel")`. Never send mutations before the corresponding
island is committed. `children` and `dangerouslySetInnerHTML` are rejected at
runtime and excluded from its TypeScript props. Keep `name` and `dom` stable for
the mounted component. Each island name is unique within its interpreter.

StrictMode's immediate effect cleanup/setup replay keeps the same loan and C++
children. Release is deferred to a microtask; a real unmount revokes the loan.
Unexpected synchronous release errors are caught and logged, and the adapter drops
its local lease. Such an error still requires application-level recovery/reload;
logging it does not acknowledge a C++ revocation. A bridge already closed rejects
the interpreter's revocation Promise, which the interpreter handles and logs.
Parent renders preserve the C++ subtree. A later real mount gets fresh node IDs.
Delayed effect disconnect/reconnect mechanisms such as React
[`Activity`](https://react.dev/reference/react/Activity) or Offscreen are unsupported
for a live `Island`: hiding revokes its loan, and revealing preserved DOM does not
restore it. Coordinate a full unmount, fresh empty mount and C++ rebuild instead.
Unmount still requires the [document handoff](DOM.md#coordinating-page-unmounts):
pause all document writers, flush, await a browser round trip, then unmount and
await an application-level acknowledgement that C++ observed the revocation
before resuming. The release function itself returns no acknowledgement or Promise.
The component cannot pause application writers itself. An uncoordinated unmount can close the whole document.

## Game of Life variant

`examples/life/react/` keeps the simulation in C++, renders the grid and statistics
with React, and builds a heading, status and “Next generation” button in C++ inside `life-panel`.
The C++ event computes a generation, updates the island and sets a tuple snapshot
containing the packed grid, generation and population. Each browser owns a separate
simulation, released on disconnect. The original Life starter remains available.

Build the `WebFrontLifeReact` CMake target and run it with a writable copy of
`main.tsx` and `webfront.project.json` from that directory as its project argument
(without argument it uses `./life-project`, created when missing).
In its terminal, run `wf install`, then `wf preview`. The configured registry must
carry `@webfront/react@0.1.0`; this change prepares the archive but does not publish
to npm. The repository's test registry already carries the package. For a local
evaluation, configure the driver with `--registry-dir <build>/test/web/self-build-project/build-registry`;
the browser suite uses that registry for this adapter and React. To use the same
registry in `WebFrontLifeReact`, copy its contents to `<project>/registry/` and add
`"registry": "/registry/"` to the project manifest before `wf install`. This is a
development mirror served from the project document root.

To prepare the archive, use the repository wf project after restoring its packages:

```sh
wf run react-package
```

This writes `build-react-package/webfront-react-0.1.0.tgz` and `integrity.txt`.
Publishing that archive to a registry is a separate maintainer action.

The test registry uses the same deterministic packager as `wf run react-package`.
`ReactAdapterPackageContents` verifies byte-for-byte archive equality, the exact
file list, every maintained source file and the SHA-512 in `integrity.txt`.
The Life canvas ignores clicks until `life.react.start` acknowledges startup. If
the React island does not mount within five seconds, startup fails visibly.
Editing a cell starts a new simulation at generation zero.

The generated `.tgz` uses valid gzip stored blocks, without size compression, so
bootstrap packaging needs no preinstalled compressor. Registry publication is a
separate step and may retain this format or recompress with a newly recorded digest.
The canvas cell editor is pointer-only; the C++ “Next generation” button supports
normal keyboard activation. A complete keyboard cell editor is not demonstrated.
