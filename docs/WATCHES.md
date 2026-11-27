# Watches: reading and writing C++ objects from the inspectors

A watch names an object or a container of your application. The **C++ state ▸ Watches** panel of the
[developer environment](DEVELOPER_ENVIRONMENT.md) shows it as a tree, unfolded level by level and refreshed while you use the
interface. It needs no debugger, no pause of the program and no `printf`. The contracts are those of
[ADR-027](adr/ADR-027-inspecteurs-de-l-etat-de-l-application.md), sections 3 to 6.

```cpp
#include <webfront/WebFront.hpp>

std::mutex               mutex;
std::vector<std::string> grid;       // written by another thread
std::uint64_t            generations = 0;

app.bootstrap("developer");
auto gridWatch = app.watch("grid", [&](webfront::inspect::Visit<const std::vector<std::string>> visit) {
    std::scoped_lock lock{mutex};
    visit(grid);
});
auto countWatch = app.watch("generations", [&](webfront::inspect::Visit<std::uint64_t> visit) {
    std::scoped_lock lock{mutex};
    visit(generations);              // a Visit<T> of a non-constant T: the inspector can write it
});
```

`examples/life/LifeWatches.hpp` is this example, compiled in `WebFrontLifeDeveloper`.

A watch exists while its handle (`webfront::inspect::Watch`) lives. Declare the watches **after** `bootstrap("developer")`:
outside the developer environment, and in a distribution build (`WEBFRONT_DISTRIBUTION=ON`), `watch` registers nothing
and returns an empty handle, so an application does not surround its watches with `#if`. A name is unique while its handle
lives and takes 256 bytes at most (`std::invalid_argument` otherwise). Each `watch` records the file and the line of its
call, which the panel shows.

This is a development tool, not a sandbox. Only a page that is admitted reads or writes a watch: the view of an embedded
frontend, or a loopback page when the application chose `trustLoopbackClients` (see
[the menu and the inspectors](DEVELOPER_ENVIRONMENT.md#the-page-interface-and-floating-terminal)). A watch is
read-only unless its source writes, and only the panel writes it: the [MCP reads](#reading-a-watch-from-an-agent) never do.
The application's own callbacks follow [their own contract](DEVELOPER_ENVIRONMENT.md#the-inspectors-and-the-applications-callbacks).

## Sources: one operation, one access

Every operation of the inspector (a page of children, a pass of differences, a full value, a write) runs **whole** inside
one access to the object, which the source defines. No reference to the object leaves it.

| Source | Form | Use it for | Writes |
| --- | --- | --- | --- |
| Visitor | `[&](Visit<const T> visit) { std::scoped_lock lock{m}; visit(object); }` | an object shared between threads: the operation runs during `visit`, under your lock | with `Visit<T>` |
| Reference on an executor | `watch(name, [&] -> T& { return object; }, app.uiExecutor())` | an object that one thread owns: the accessor and the operation run on that thread | with a non-constant reference |
| Reference without an executor | `[&] -> const T& { return object; }` | an object that only the server thread touches (the thread of the `cppFunction` callbacks) | never |
| Copy | `[&] { return object; }` | a small object: it is copied at each operation, and none of the costs below is bounded | never |
| Owning source | `watch(name, std::shared_ptr<State>, source)` | an order of destruction you cannot guarantee (see [Destroying the handle](#destroying-the-handle)) | as its form |

A source calls its `visit` once, with the object. A source that does not call it makes the operation fail with
`webfront.inspector.unavailable`; one that throws before calling it rejects the call, with nothing changed; one that throws
after it is written to the diagnostics, and the answer is served.

**A lock taken inside an accessor protects nothing.** `[&] -> const T& { std::scoped_lock lock{m}; return object; }` releases
the lock before the object is read. That is the mistake the visitor avoids.

**Executor.** An executor is anything whose `schedule()` returns a sender that completes on the thread owning the object:
`app.uiExecutor()`, the loop of a [host](HOST_INTEGRATION.md), or your own. The calls of a connection on a watch run one by
one, in the order they were received; the executor must run what it is given in order, as a thread's loop does. A watch
whose executor has no running loop answers `webfront.inspector.unavailable`.

## What is shown

The kind and the value of a node are chosen in this order:

| Kind | Types | Value | Children |
| --- | --- | --- | --- |
| `bool`, `integer`, `real` | `bool`, integers, floating-point numbers | the value | none |
| `string` | `std::string`, `std::string_view` | the text | none |
| `bitset` | `std::bitset<N>` | N binary digits, most significant first | none |
| `optional` | `std::optional<T>` | `engaged` or `nullopt` | the value, when there is one |
| `variant` | `std::variant<...>` | `alternative <index>` | the alternative it holds |
| `tuple` | `std::pair`, `std::tuple` | empty | its members |
| `sequence` | any `std::ranges::sized_range`, a `PagedSource` | empty | its elements, labelled `[index]` or by their key |
| `map` | associative containers (`key_type` and `mapped_type`) | empty | its mapped values, labelled by their formatted key |
| `described` | a type with a description (below) | the name of the description | one per field; a choice has the kind `enum` |
| `formatted` | a type `std::format` writes | the formatted text | none |
| `opaque` | anything else | the type's name and "(not displayable)" | none |

Control characters are shown as their visible pictures (`␊` for a line feed). A label and a value take 256 bytes at most:
a longer value is cut, flagged, and its node offers **Full value**, which reads it by fragments up to 16 MiB.

**Described types.** Give a type the description that [forms](FORMS.md) use, and its fields become nodes, each with its
label and, when written, its bounds and choices:

```cpp
template <>
struct webfront::inspect::Described<Settings> {
    static webfront::forms::Description<Settings> description() {
        using namespace webfront::forms;
        return {.name = "Settings", .fields = {field("speed", &Settings::speed, "Speed", Bounds{1, 10})}};
    }
};
```

With C++26 reflection (GCC 16, `components/forms/Reflection.hpp`), `description()` returns `forms::describe<Settings>()`. A
field's value is read through its accessor, which copies a text member whole.

**Formatters.** WebFront never formats a value whole to cut it afterwards: a value is written into a bounded receiver, a
string copied up to the bound only. For the kinds it renders itself, the memory of a value is bounded. For `formatted`,
it bounds what it stores of the output, not what your `std::formatter` allocates or how long it runs, and it runs **under
your source's lock**. Describe a type whose formatting is costly, or page it, rather than rely on its formatter. An
exception thrown by a formatter becomes the value of its node (`‹exception : message›`, kind `opaque`).

## Large containers

With a visitor or a reference, no operation copies the object, and the memory and the message of an operation are
proportional to the page (200 children by default, 1 000 at most, fewer when the answer's budget is reached). The time
depends on the container:

- a `std::ranges::random_access_range` reaches a position at once: a page costs its size;
- another range (`std::list`, `std::map`) is walked from its start to the position: a late page of a very large one is
  linear, **under your lock**;
- a `PagedSource` pages itself, at the cost you decide.

```cpp
struct Rows {                                   // webfront::inspect::PagedSource
    std::size_t size() const;
    void visit(std::size_t first, std::size_t count, auto&& each);   // each(element), in order
    std::string key(std::size_t position) const;                     // optional, with find()
    std::optional<std::size_t> find(std::string_view key) const;
};
```

`key` and `find` give the elements an identity. The contract is a primary key's: a key names at most one element, and an
element keeps its key while it exists. The path of an element then carries its key, so the inspector follows it when the
container changes, and a write reaches the element it was typed for. WebFront checks what is local: the key at the
position `find` returns must be the same key (else the node is gone), and two elements of one page with the same key are
shown by their position, not writable, with one diagnostic. A key whose percent-encoding passes 128 bytes also falls back
to the position.

Without keys an element is known by its position. When the container changes between two reads the inspector may show a
neighbour for a moment; the next pass corrects it, and a list read in several pages starts again when the container
changed between two of them.

A tree is unfolded down to 64 levels, and each connection keeps 10 000 nodes of a watch at most: beyond, the panel asks you
to fold some.

## Writing a value

A watch is read-only unless its source writes: a `Visit<T>` of a non-constant `T`, or a non-constant reference on an
executor. A copy never writes. In a writable watch the panel edits in place (Enter writes, Escape cancels) the booleans,
numbers, `std::string`, `std::bitset<N>` (N binary digits) and the fields of a described type, within their bounds and
choices. A node whose label or value is cut, a text that holds a control character (shown as its picture, which an edition
would write back in its place), a constant member, and a keyed element shown by its position are not writable.

A write carries what the panel displayed. Inside the same access as the assignment, WebFront checks that the target **and
each of its ancestors** still show it (label, value, number of children); otherwise the write is refused as stale, the
object is left as it is, and the panel reads the node again. It is a compare-and-swap on what you saw. It cannot tell two
elements without keys that look the same in every visible way: give such a container keys when the identity matters.

The answer of a write carries no value, and the inspector's memory keeps what it was sent: the next pass, or a reading of
the node, tells the value the object holds, also when a mutator stored another value than the text.

To keep an invariant, pass a mutator, which replaces the assignment and throws to refuse:

```cpp
auto watch = app.watch("settings", source, [](Settings& settings, std::string_view path, std::string_view text) {
    if (path == "/.0" && text == "0")
        throw std::invalid_argument("The speed cannot be 0");       // shown beside the value, bounded to 256 bytes
    apply(settings, path, text);                                    // all or nothing: that is the mutator's contract
});
```

`path` tells the node: each segment is `/` then `#` and an index, `.` and the rank of a described field, or `k` and a
percent-encoded key (`webfront::inspect::parsePath` decodes it). `text` was already checked against the node's kind.

| Refusal | Meaning |
| --- | --- |
| `webfront.inspector.parse` | the text is not a value of the node, or is out of its bounds |
| `webfront.inspector.readOnly` | the node is not writable |
| `webfront.inspector.tooLarge` | the text is longer than the bound of the buffers policy |
| `webfront.inspector.stale` | what was displayed is no longer true |
| `webfront.inspector.refused` | the mutator, or the member itself, refused the value |

## Destroying the handle

A source usually captures references to your objects (the object, its mutex). Destroying the handle must guarantee that no
operation touches them afterwards.

- **By default the destruction waits** for an operation that another thread started, as joining a thread does. Destroy the
  handle **outside every lock its source takes**, and outside the watch's executor; otherwise the operation waits for the
  lock held by the thread that waits for the operation. Declaring the handle after the mutex is enough: it is destroyed
  first. A destruction that waits for more than five seconds writes a diagnostic that names the watch and where it was
  declared, then goes on waiting.
- **From the operation itself** (the source, the visitor, the mutator) the destruction can neither wait nor let the
  operation go on over objects you may destroy next: WebFront writes the diagnostic and calls `std::terminate`.
- **An owning source never waits.** Give `watch` a `std::shared_ptr` to the state the source uses; each operation keeps it
  alive, so the handle may be destroyed under the source's lock and from the operation itself:

```cpp
struct Stats { std::mutex mutex; std::map<std::string, double> values; };
auto state = std::make_shared<Stats>();
auto watch = app.watch("stats", state, [](Stats& stats, webfront::inspect::Visit<const std::map<std::string, double>> visit) {
    std::scoped_lock lock{stats.mutex};
    visit(stats.values);
});
```

Once a watch is removed, an operation in progress ends, and its answer, like every later call, is
`webfront.inspector.unknown`; the panel removes the watch. A write that was already applied says so in that refusal.

## Bounds

| Bound | Value |
| --- | --- |
| Answer | `responseBudget = min(functionCallBufferSize, maxMessageSize)` of the buffers policy, 32 KiB by default; at least 4 KiB or the developer environment does not compile |
| Text of a write | `maxWriteTextBytes`: what `maxMessageSize` leaves beside the other arguments, at least 1 KiB; checked by the page, then by C++ |
| Label, value, name, message of an exception | 256 bytes of UTF-8, cut on a character |
| Path | 1 024 bytes and 64 segments; a key, 128 bytes once encoded |
| Memory of a connection for a watch | 10 000 nodes |
| Full value | 16 MiB, read by fragments of one snapshot |
| Page | 200 entries by default, 1 000 at most |

## How the panel reads

The panel reads at a period you choose (250 ms by default) and can be paused; C++ sends nothing by itself, and a watch
that no inspector reads costs an entry in a table. Each period is a *pass of differences*: C++ compares what it sent to
this connection with the object, in one access, and answers the nodes that changed, by pages. An unfolding, a folding or a
write never waits for the end of a pass: it interrupts it, and the next pass finds the rest. The protocol
(`webfront.inspector.children`, `changes`, `value`, `fold`, `reset`, `limits`, `write`) is described in section 3 of the
ADR; it uses the bridge's existing types only.

## Reading a watch from an agent

An application that enables [automation](AUTOMATION.md#reading-the-applications-state) with the `state` grant gives an MCP
client the same reads as tools: `webfront.state.watches`, `children`, `changes`, `value`, `fold`, `reset` and `limits`. They
run the same operations, in the same access window of the source and with the same bounds, on a memory of their own that
no panel shares. They never write: a watch you made writable for the panel stays read-only for an agent.
[Developing with an agent](DEVELOPING_WITH_AGENTS.md#reading-the-running-application-webfrontstate) gives the sequence.
