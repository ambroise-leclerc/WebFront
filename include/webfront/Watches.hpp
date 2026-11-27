// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — core
// Role: The watches an application declares for the inspectors (ADR-027): their sources, their registry, their handle and their operations.
// Thread: thread-safe | Alloc: unrestricted
// LLM: public API (Watch, Visit, PagedSource in WatchValues.hpp); the registry answers webfront.inspector.watches, children, changes, value, fold, reset and
// write

#pragma once

#include "Task.hpp"
#include "WatchMemory.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace webfront::inspect {

// --- Sources (ADR-027, section 5) ------------------------------------------------------------------------------

namespace detail {
template <typename T>
struct visit_object { };
template <typename T>
struct visit_object<Visit<T>> {
    using type = T;
};

/// The parameters of a call signature, as a tuple.
template <typename Signature>
struct parameter_list { };
template <typename R, typename... A>
struct parameter_list<R (*)(A...)> {
    using type = std::tuple<A...>;
};
template <typename R, typename... A>
struct parameter_list<R (*)(A...) noexcept> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...)> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) const> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) noexcept> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) const noexcept> : parameter_list<R (*)(A...)> { };
// Callable on an lvalue only, as the stored source is invoked.
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) &> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) const&> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) & noexcept> : parameter_list<R (*)(A...)> { };
template <typename R, typename C, typename... A>
struct parameter_list<R (C::*)(A...) const & noexcept> : parameter_list<R (*)(A...)> { };

template <typename Source>
struct call_signature {
    using type = Source;  // a function pointer
};
template <typename Source>
    requires requires { &Source::operator(); }
struct call_signature<Source> {
    using type = decltype(&Source::operator());
};

/// The object a visitor visits, from the parameters of its source: (Visit<T>), or (State&, Visit<T>) for an owning source.
template <typename Parameters>
struct visited { };
template <typename V>
    requires requires { typename visit_object<std::remove_cvref_t<V>>::type; }
struct visited<std::tuple<V>> {
    using type                     = typename visit_object<std::remove_cvref_t<V>>::type;
    static constexpr bool stateful = false;
};
template <typename S, typename V>
    requires requires { typename visit_object<std::remove_cvref_t<V>>::type; }
struct visited<std::tuple<S, V>> {
    using type                     = typename visit_object<std::remove_cvref_t<V>>::type;
    static constexpr bool stateful = true;
};

template <typename Source>
concept visitor_source = requires { typename visited<typename parameter_list<typename call_signature<Source>::type>::type>::type; }
                         && !visited<typename parameter_list<typename call_signature<Source>::type>::type>::stateful;

template <typename Source>
concept stateful_visitor_source = requires { typename visited<typename parameter_list<typename call_signature<Source>::type>::type>::type; }
                                  && visited<typename parameter_list<typename call_signature<Source>::type>::type>::stateful;
}  // namespace detail

/**
 * @brief How a watch reaches its object, deduced from its source: a visitor (`[&](Visit<const T> visit) { ... visit(object); }`),
 * or an accessor that returns a reference or a copy (`[&] -> const T& { return object; }`). A visitor of a non-constant
 * object is writable; an accessor is writable only when it returns a non-constant reference and the watch has an executor,
 * and a copy never is: it is not the object (ADR-027, section 6).
 */
template <typename Source>
struct SourceShape;

template <typename Source>
    requires detail::visitor_source<Source>
struct SourceShape<Source> {
    using Object                   = typename detail::visited<typename detail::parameter_list<typename detail::call_signature<Source>::type>::type>::type;
    static constexpr bool visitor  = true;
    static constexpr bool writable = !std::is_const_v<Object>;
};

// An accessor gives an object: one that returns nothing (void) gives nothing to show, and is refused.
template <typename Source>
    requires(!detail::visitor_source<Source> && std::invocable<Source&> && !std::is_void_v<std::invoke_result_t<Source&>>)
struct SourceShape<Source> {
    using Object                   = std::remove_reference_t<std::invoke_result_t<Source&>>;
    static constexpr bool visitor  = false;
    static constexpr bool writable = false;
};

template <typename Source>
concept WatchSource = requires { typename SourceShape<std::decay_t<Source>>::Object; };

/**
 * @brief The shape of an owning source (ADR-027, section 5): the watch keeps a `std::shared_ptr<State>` alive during each
 * operation and the source receives the state, `[](State& state, Visit<T> visit) { std::scoped_lock lock{state.mutex};
 * visit(state.value); }` or `[](State& state) -> T& { return state.value; }`. A source of the ordinary shapes is accepted
 * too, when it captures nothing that the state does not keep alive.
 */
template <typename Source, typename State>
struct OwnedSourceShape;

template <typename Source, typename State>
    requires detail::stateful_visitor_source<Source>
             && std::invocable<Source&,
                               State&,
                               Visit<typename detail::visited<typename detail::parameter_list<typename detail::call_signature<Source>::type>::type>::type>>
struct OwnedSourceShape<Source, State> {
    using Object                   = typename detail::visited<typename detail::parameter_list<typename detail::call_signature<Source>::type>::type>::type;
    static constexpr bool visitor  = true;
    static constexpr bool stateful = true;
    static constexpr bool writable = !std::is_const_v<Object>;
};

template <typename Source, typename State>
    requires(!detail::stateful_visitor_source<Source> && !detail::visitor_source<Source> && std::invocable<Source&, State&>
             && !std::is_void_v<std::invoke_result_t<Source&, State&>>)
struct OwnedSourceShape<Source, State> {
    using Object                   = std::remove_reference_t<std::invoke_result_t<Source&, State&>>;
    static constexpr bool visitor  = false;
    static constexpr bool stateful = true;
    static constexpr bool writable = false;
};

template <typename Source, typename State>
    requires(!detail::stateful_visitor_source<Source> && !std::invocable<Source&, State&> && WatchSource<Source>)
struct OwnedSourceShape<Source, State> : SourceShape<Source> {
    static constexpr bool stateful = false;
};

template <typename Source, typename State>
concept OwnedWatchSource = requires { typename OwnedSourceShape<std::decay_t<Source>, State>::Object; };

/// The executor of a watch: what owns the object's thread (`app.uiExecutor()`, a host's loop). `schedule()` gives a sender
/// that completes there; the operations of a watch are run in the order they are scheduled.
template <typename Executor>
concept WatchExecutor = std::copy_constructible<Executor> && requires(const Executor& executor) { executor.schedule(); };

/// The application's own way to write a value of a watch of T: `mutate(object, path, text)`, which throws to refuse it.
template <typename Mutator, typename Object>
concept WatchMutator = !std::is_const_v<Object> && std::invocable<Mutator&, Object&, std::string_view, std::string_view>;

// --- Registry --------------------------------------------------------------------------------------------------

/// A watch as webfront.inspector.watches describes it: (identifier, name, kind of the root, "file:line", writable).
using Descriptor = std::tuple<double, std::string, std::string, std::string, bool>;
/// A page of descriptors: (descriptors, next, revision of the registry, changed). `next` is 0 at the end.
using WatchPage = std::tuple<std::vector<Descriptor>, double, double, bool>;

namespace detail {

/// Runs `work` on the executor's thread; the task itself resumes where it was awaited.
template <typename Executor>
ResultTask<void> hopOn(Executor executor, std::function<void()> work) {
    co_await networking::ex::then(executor.schedule(), std::move(work));
}

/// A registered watch. Its operations hold it, so that a source that owns its state outlives a handle destroyed meanwhile.
struct WatchCore {
    std::uint64_t id = 0;
    std::string   name;           // as the application gave it: what makes a watch unique
    std::string   kind;
    std::string   location;       // "file:line", visible and bounded
    bool          owned = false;  // the source keeps its state alive: destroying the handle never waits
    Access        access;
    /// Runs an operation on the watch's executor; empty when the operations run on the thread that asks.
    std::function<ResultTask<void>(std::function<void()>)> hop;
    std::shared_ptr<void>                                  keep;  // what a source registered without access captures

    std::mutex                   lifecycle;
    std::condition_variable      idle;
    std::vector<std::thread::id> running;  // the threads inside an operation
    bool                         removed = false;
};

/// What a connection reads of a full value: one at a time, told by an identifier never reused while the connection lives.
struct Snapshot {
    std::uint64_t id    = 0;
    std::uint64_t watch = 0;
    std::string   path;
    std::string   text;
    std::size_t   next   = 0;
    bool          capped = false;
};

}  // namespace detail

/**
 * @brief What the inspectors keep for one connection: its memory of each watch and its snapshot of a full value. Freed
 * when the connection closes (WatchRegistry::forget).
 */
class WatchSession {
public:
    /// The memory of a watch for this connection, created empty at its first call.
    [[nodiscard]] std::shared_ptr<detail::Memory> memory(std::uint64_t watch) {
        std::scoped_lock lock{mutex};
        if (closed)
            throw webfront::detail::makeBridgeError(bridge_code::disconnected, "The connection is closed");
        auto& kept = memories[watch];
        if (!kept)
            kept = std::make_shared<detail::Memory>();
        return kept;
    }
    /// The memory of a watch if this connection has one: for tests and diagnostics.
    [[nodiscard]] std::shared_ptr<detail::Memory> find(std::uint64_t watch) const {
        std::scoped_lock lock{mutex};
        const auto       found = memories.find(watch);
        return found == memories.end() ? nullptr : found->second;
    }
    [[nodiscard]] bool hasSnapshot() const {
        std::scoped_lock lock{mutex};
        return snapshot.has_value();
    }
    /// Frees what this connection keeps of a watch: its memory and, when it is the watch's, the snapshot.
    void drop(std::uint64_t watch) noexcept {
        std::shared_ptr<detail::Memory> released;
        std::optional<detail::Snapshot> snapshotReleased;
        std::scoped_lock                lock{mutex};
        if (const auto found = memories.find(watch); found != memories.end()) {
            released = std::move(found->second);
            memories.erase(found);
        }
        if (snapshot && snapshot->watch == watch)
            snapshotReleased.swap(snapshot);
    }

private:
    friend class WatchRegistry;
    mutable std::mutex                                       mutex;
    bool                                                     closed = false;
    std::map<std::uint64_t, std::shared_ptr<detail::Memory>> memories;
    std::uint64_t                                            snapshots = 0;
    std::optional<detail::Snapshot>                          snapshot;
};

/**
 * @brief The watches of an application (ADR-027). Each registration gets an identifier that is never reused while the
 * registry lives, and the registry's revision changes at each registration and each removal. Discovering the watches
 * (page) calls no source; the other operations read or write the watched object, each whole inside the access window of
 * its source. Thread-safe.
 */
class WatchRegistry {
public:
    using Core = detail::WatchCore;

    /// Where the registry tells the application what it should know (ADR-011): standard error while it is empty. Set it
    /// before the watches are registered.
    std::function<void(std::string_view)> diagnostic;
    /// How long the destruction of a handle waits for an operation before it says so (and goes on waiting).
    std::chrono::milliseconds destructionWarning{5000};

    /// Registers a watch; throws std::invalid_argument for an empty name, a name longer than maxTextBytes, a kind longer
    /// than maxKindBytes, or the name of a watch that is still registered. The location is made visible and bounded as a
    /// text of the application is, so that every descriptor fits a page of the smallest budget.
    std::uint64_t add(std::shared_ptr<Core> core) {
        checkName(core->name);
        if (core->kind.empty() || core->kind.size() > maxKindBytes)
            throw std::invalid_argument("A watch's kind is 1 to 16 bytes");
        core->access.label       = boundedText(core->name);
        core->location           = boundedText(core->location);
        core->access.diagnose    = diagnostic ? diagnostic : std::function<void(std::string_view)>{&toStandardError};
        auto             keeping = core;  // declared before the lock: a source released by a failed insertion is destroyed after it
        std::scoped_lock lock{mutex};
        for (const auto& [id, entry] : entries)
            if (entry->name == core->name)
                throw std::invalid_argument("A watch named '" + core->name + "' is already registered");
        const auto id = nextId++;
        core->id      = id;
        entries.emplace(id, std::move(core));
        ++currentRevision;
        return id;
    }

    /// Registers a watch that has no access to its object: it is listed, and its operations answer `unavailable`.
    std::uint64_t add(std::string name, std::string_view kind, std::string location, bool writable, std::shared_ptr<void> source) {
        auto core             = std::make_shared<Core>();
        core->name            = std::move(name);
        core->kind            = std::string(kind);
        core->location        = std::move(location);
        core->access.writable = writable;
        core->owned           = true;
        core->keep            = std::move(source);
        return add(std::move(core));
    }

    /**
     * Removes a watch; a removed or unknown identifier is ignored. From then on no operation starts, and the calls that
     * name it are refused (`webfront.inspector.unknown`). A watch whose source refers to objects of the application waits
     * for an operation that another thread started: call it outside every lock the source takes. Removing such a watch
     * from its own operation cannot be made safe: it is reported, then the process is terminated (ADR-027, section 5).
     */
    void remove(std::uint64_t id) noexcept {
        std::shared_ptr<Core> core;
        {
            std::scoped_lock lock{mutex};
            const auto       found = entries.find(id);
            if (found == entries.end())
                return;
            core = std::move(found->second);
            entries.erase(found);
            ++currentRevision;
            // Closed in the same section as the erasure: an operation that found the watch before cannot start once the
            // registry tells it is gone.
            std::scoped_lock closing{core->lifecycle};
            core->removed = true;
        }
        retire(*core);
        std::vector<std::shared_ptr<WatchSession>> open;
        {
            std::scoped_lock lock{sessionsMutex};
            for (const auto& [connection, session] : sessions)
                open.push_back(session);
        }
        std::vector<std::shared_ptr<detail::Memory>> released;  // freed outside the sessions' locks
        for (const auto& session : open) {
            std::scoped_lock lock{session->mutex};
            if (const auto found = session->memories.find(id); found != session->memories.end()) {
                released.push_back(std::move(found->second));
                session->memories.erase(found);
            }
            if (session->snapshot && session->snapshot->watch == id)
                session->snapshot.reset();
        }
        // The source is released here, outside the registry's lock, unless an operation still holds it.
    }

    [[nodiscard]] std::uint64_t revision() const {
        std::scoped_lock lock{mutex};
        return currentRevision;
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock{mutex};
        return entries.size();
    }

    [[nodiscard]] bool contains(std::uint64_t id) const {
        std::scoped_lock lock{mutex};
        return entries.contains(id);
    }

    /**
     * The page that webfront.inspector.watches(revision, after, count) answers: the live watches whose identifier is
     * greater than `after`, in the order of the identifiers, at most `count` of them (0 asks only for the revision) and as
     * many as fit `budget` bytes with the answer's header. `next` is the last identifier served, 0 at the end; `changed`
     * tells that `revision` is no longer the registry's. Throws std::invalid_argument for a number that is not a whole
     * number from 0 to 2^53, and std::length_error when the first remaining watch does not fit `budget`: an empty page
     * ending with 0 would hide the rest (a budget of minPageBytes always holds one).
     */
    [[nodiscard]] WatchPage page(double revision, double after, double count, std::size_t budget) const {
        const auto              known = wholeNumber(revision, "revision"), from = wholeNumber(after, "cursor");
        const auto              wanted = std::min<std::uint64_t>(wholeNumber(count, "count"), maxPageCount);
        const auto              room   = budget > responseHeaderBytes ? budget - responseHeaderBytes : 0;
        std::scoped_lock        lock{mutex};
        std::vector<Descriptor> served;
        std::size_t             used = 0;
        std::uint64_t           last = 0;
        bool                    more = false;
        for (auto entry = entries.upper_bound(from); entry != entries.end(); ++entry) {
            const auto& core  = *entry->second;
            const auto  bytes = encodedSize(core);
            if (served.size() == wanted || used + bytes > room) {
                if (served.empty() && wanted != 0)
                    throw std::length_error("The inspector's page budget cannot hold one watch descriptor");
                more = true;
                break;
            }
            used += bytes;
            last  = entry->first;
            served.emplace_back(static_cast<double>(entry->first), core.access.label, core.kind, core.location, core.access.writable);
        }
        const bool changed = known != currentRevision;
        return {std::move(served), more && wanted != 0 ? static_cast<double>(last) : 0.0, static_cast<double>(currentRevision), changed};
    }

    /// The largest number of bytes a descriptor takes encoded: the bound that makes a page of one always fit minPageBytes.
    static constexpr std::size_t maxDescriptorBytes = 2 + 9 + (3 + maxTextBytes) + (3 + maxKindBytes) + (3 + maxTextBytes) + 1;
    static_assert(maxDescriptorBytes + responseHeaderBytes <= minPageBytes);

    static void checkName(std::string_view name) {
        if (name.empty() || name.size() > maxTextBytes)
            throw std::invalid_argument("A watch's name is 1 to 256 bytes");
    }

private:
    [[nodiscard]] std::shared_ptr<Core> find(double watch) const {
        const auto       id = wholeNumber(watch, "watch");
        std::scoped_lock lock{mutex};
        const auto       found = entries.find(id);
        if (found == entries.end())
            throw gone(false);
        return found->second;
    }

    /// The terminal answer of a watch that is gone (ADR-027, "Réponse terminale").
    [[nodiscard]] static BridgeError gone(bool written) {
        return detail::refusal(code::unknown, written ? "The watch was removed after the write" : "No such watch: it was removed, or never existed");
    }

    /**
     * Runs an operation of a watch. None starts once the watch is removed, and the answer of one that was running when
     * it was removed is replaced by the terminal refusal: its revision would be that of a memory that no longer exists.
     */
    template <typename Operation>
    static auto perform(Core& core, Operation&& operation, const bool* written = nullptr, WatchSession* session = nullptr) {
        {
            std::scoped_lock lock{core.lifecycle};
            if (core.removed)
                throw gone(false);
            core.running.push_back(std::this_thread::get_id());
        }
        struct Done {
            Core& core;
            ~Done() {
                std::scoped_lock lock{core.lifecycle};
                core.running.erase(std::ranges::find(core.running, std::this_thread::get_id()));
                core.idle.notify_all();
            }
        };
        // A watch removed while the operation ran: what the operation kept for the connection (a memory it created again)
        // is freed, since the removal may have cleaned the session before.
        const auto removed = [&] {
            {
                std::scoped_lock lock{core.lifecycle};
                if (!core.removed)
                    return false;
            }
            if (session != nullptr)
                session->drop(core.id);
            return true;
        };
        Done done{core};
        try {
            auto result = operation();
            if (removed())
                throw gone(written != nullptr && *written);
            return result;
        } catch (...) {
            if (removed())
                throw gone(written != nullptr && *written);
            throw;
        }
    }

    /// Whether the watch is being removed. Read under a session's lock, it orders a publication in that session with the
    /// removal, which takes the session's lock after it marks the watch.
    [[nodiscard]] static bool removing(Core& core) {
        std::scoped_lock lock{core.lifecycle};
        return core.removed;
    }

public:
    // --- Operations (ADR-027, sections 3 and 6) ----------------------------------------------------------------

    /// What the inspectors keep for a connection, created at its first call. `connection` is never reused while the
    /// registry lives.
    [[nodiscard]] std::shared_ptr<WatchSession> session(std::uint64_t connection) {
        std::scoped_lock lock{sessionsMutex};
        auto&            kept = sessions[connection];
        if (!kept)
            kept = std::make_shared<WatchSession>();
        return kept;
    }

    /// Frees what the inspectors keep for a closed connection: its memories, its passes and its snapshot.
    void forget(std::uint64_t connection) noexcept {
        std::shared_ptr<WatchSession> closed;
        {
            std::scoped_lock lock{sessionsMutex};
            const auto       found = sessions.find(connection);
            if (found == sessions.end())
                return;
            closed = std::move(found->second);
            sessions.erase(found);
        }
        std::map<std::uint64_t, std::shared_ptr<detail::Memory>> released;
        std::optional<detail::Snapshot>                          snapshot;
        std::scoped_lock                                         lock{closed->mutex};
        closed->closed = true;
        released.swap(closed->memories);
        snapshot.swap(closed->snapshot);
    }

    [[nodiscard]] std::size_t sessionCount() const {
        std::scoped_lock lock{sessionsMutex};
        return sessions.size();
    }

    /// How the operations of a watch reach its executor; empty when they run on the thread that asks. Throws `unknown`.
    [[nodiscard]] std::function<ResultTask<void>(std::function<void()>)> hopOf(double watch) const {
        return find(watch)->hop;
    }

    Page children(const std::shared_ptr<WatchSession>& from,
                  double                               watch,
                  std::string_view                     path,
                  double                               revision,
                  double                               start,
                  double                               count,
                  const Limits&                        limits) {
        const auto core = find(watch);
        return perform(
            *core,
            [&] {
                return from->memory(core->id)->children(core->access, path, revision, start, count, limits);
            },
            nullptr,
            from.get());
    }

    Page changes(const std::shared_ptr<WatchSession>& from, double watch, double revision, double cursor, double count, const Limits& limits) {
        const auto core = find(watch);
        return perform(
            *core,
            [&] {
                return from->memory(core->id)->changes(core->access, revision, cursor, count, limits);
            },
            nullptr,
            from.get());
    }

    Page fold(const std::shared_ptr<WatchSession>& from, double watch, std::string_view path, double revision) {
        const auto core = find(watch);
        return perform(
            *core,
            [&] {
                return from->memory(core->id)->fold(path, revision);
            },
            nullptr,
            from.get());
    }

    double reset(const std::shared_ptr<WatchSession>& from, double watch) {
        const auto core = find(watch);
        return perform(
            *core,
            [&] {
                return from->memory(core->id)->reset();
            },
            nullptr,
            from.get());
    }

    WriteAnswer write(const std::shared_ptr<WatchSession>& from,
                      double                               watch,
                      double                               revision,
                      std::string_view                     path,
                      std::string_view                     expected,
                      std::string_view                     text,
                      const Limits&                        limits) {
        const auto core    = find(watch);
        bool       applied = false;
        return perform(
            *core,
            [&] {
                return from->memory(core->id)->write(core->access, revision, path, expected, text, limits, applied);
            },
            &applied,
            from.get());
    }

    /**
     * webfront.inspector.value: a fragment of the whole value of a node. The snapshot 0 formats the value, inside the
     * access window, and keeps it as the connection's snapshot under a new identifier; the following calls give that
     * identifier and the offset they received. Anything else is refused (`stale`): a second reading that replaced the
     * snapshot makes the rest of the first one fail, and never serves it a fragment of another value.
     */
    ValueAnswer
    value(const std::shared_ptr<WatchSession>& from, double watch, std::string_view path, double snapshotAsked, double offsetAsked, const Limits& limits) {
        const auto core = find(watch);
        return perform(
            *core,
            [&]() -> ValueAnswer {
                const auto snapshot = wholeNumber(snapshotAsked, "snapshot"), offset = wholeNumber(offsetAsked, "offset");
                const auto room = limits.responseBudget - responseHeaderBytes;
                if (snapshot == 0) {
                    if (offset != 0)
                        throw detail::refusal(code::stale, "A new reading starts at the offset 0");
                    auto [text, capped] = from->memory(core->id)->value(core->access, path, limits.maxSnapshotBytes);
                    std::scoped_lock lock{from->mutex};
                    if (from->closed)
                        throw webfront::detail::makeBridgeError(bridge_code::disconnected, "The connection is closed");
                    // A watch removed meanwhile publishes nothing: its snapshot would replace another watch's, and nothing would
                    // free it. A removal that comes after the publication frees it (it takes this lock once the watch is marked).
                    if (removing(*core))
                        throw gone(false);
                    from->snapshot = detail::Snapshot{.id     = ++from->snapshots,
                                                      .watch  = core->id,
                                                      .path   = std::string(path),
                                                      .text   = std::move(text),
                                                      .next   = 0,
                                                      .capped = capped};
                    return fragment(*from, room);
                }
                std::scoped_lock lock{from->mutex};
                const auto&      kept = from->snapshot;
                if (!kept || kept->id != snapshot || kept->watch != core->id || kept->path != path || kept->next != offset || offset == 0)
                    throw detail::refusal(code::stale, "The snapshot is no longer the connection's: read the value again");
                return fragment(*from, room);
            },
            nullptr,
            from.get());
    }

    /// The bounds of the operations, as webfront.inspector.limits answers them.
    [[nodiscard]] static LimitsAnswer limits(const Limits& limits) noexcept {
        return {static_cast<double>((std::min)(limits.maxWriteTextBytes, static_cast<std::size_t>(maxExactNumber))),
                static_cast<double>(limits.responseBudget),
                static_cast<double>(limits.maxNodes),
                static_cast<double>(limits.maxSnapshotBytes),
                static_cast<double>(maxPageCount),
                static_cast<double>(defaultPageCount)};
    }

private:
    static std::uint64_t wholeNumber(double value, std::string_view what) {
        return detail::wholeNumber(value, what);
    }

    /// An upper bound of the encoded size of a descriptor: a tuple header, a number, three strings with the longest string
    /// header and a boolean.
    static std::size_t encodedSize(const Core& core) noexcept {
        return 2 + 9 + (3 + core.access.label.size()) + (3 + core.kind.size()) + (3 + core.location.size()) + 1;
    }

    /// Marks the watch removed and, for a source that refers to objects of the application, waits for its operations.
    void retire(Core& core) noexcept {
        std::unique_lock lock{core.lifecycle};
        core.removed = true;
        if (core.owned)
            return;
        if (std::ranges::find(core.running, std::this_thread::get_id()) != core.running.end()) {
            const auto message = "The watch '" + core.access.label + "' declared at " + core.location
                                 + " was destroyed from its own operation (source, visitor or mutator): its source refers to objects that may be"
                                   " destroyed next. Use an owning source, watch(name, std::shared_ptr<State>, source), or destroy the handle"
                                   " from another thread.";
            report(message);
            if (diagnostic)
                toStandardError(message);  // also where a process that ends is read, whatever the application's journal keeps
            std::fflush(stderr);
            std::terminate();
        }
        bool warned = false;
        while (!core.running.empty()) {
            if (warned)
                core.idle.wait(lock);
            else if (core.idle.wait_for(lock, destructionWarning) == std::cv_status::timeout && !core.running.empty()) {
                warned = true;
                report("The destruction of the watch '" + core.access.label + "' declared at " + core.location + " has been waiting for an operation for "
                       + std::to_string(destructionWarning.count()) + " ms: destroy the handle outside every lock its source takes, and outside its executor");
            }
        }
    }

    static void toStandardError(std::string_view message) {
        std::fprintf(stderr, "webfront: %.*s\n", static_cast<int>(message.size()), message.data());
    }

    void report(std::string_view message) const noexcept {
        try {
            if (diagnostic)
                diagnostic(message);
            else
                toStandardError(message);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a diagnostic that fails is not worth an exception in a destructor
        }
    }

    /// The next fragment of the session's snapshot, of at most `room` bytes and never cutting a character. The session's
    /// lock is held.
    [[nodiscard]] static ValueAnswer fragment(WatchSession& from, std::size_t room) {
        auto&      kept = *from.snapshot;
        const auto rest = std::string_view{kept.text}.substr(kept.next);
        auto       part = utf8Prefix(rest, room);
        if (part.empty() && !rest.empty())
            part = rest.substr(0, (std::min)(room, rest.size()));  // bytes that are no UTF-8 text are cut where the room ends
        const auto  end  = kept.next + part.size();
        const bool  last = end == kept.text.size();
        ValueAnswer answer{std::string(part),
                           static_cast<double>(kept.text.size()),
                           last ? 0.0 : static_cast<double>(end),
                           kept.capped,
                           static_cast<double>(kept.id)};
        kept.next = end;
        if (last)
            from.snapshot.reset();
        return answer;
    }

    mutable std::mutex                                     mutex;
    std::map<std::uint64_t, std::shared_ptr<Core>>         entries;
    std::uint64_t                                          nextId          = 1;
    std::uint64_t                                          currentRevision = 1;  // never 0: an inspector that starts with 0 sees a change
    mutable std::mutex                                     sessionsMutex;
    std::map<std::uint64_t, std::shared_ptr<WatchSession>> sessions;
};

/**
 * Runs an operation of a watch where its object lives: on the watch's executor when it has one, else on the calling thread.
 * The operations of a watch are run in the order they are asked. A watch whose executor has no running loop answers
 * `webfront.inspector.unavailable`. The operation is type-erased: the coroutine's frame then names no type of the caller.
 */
template <typename Result>
ResultTask<Result> onWatch(std::shared_ptr<WatchRegistry> registry, double watch, std::function<Result()> operation) {
    const auto hop = registry->hopOf(watch);
    if (!hop)
        co_return operation();
    std::optional<Result> result;
    std::exception_ptr    failure;
    std::string           unavailable;
    try {
        co_await hop([&] {
            try {
                result.emplace(operation());
            } catch (...) {
                failure = std::current_exception();
            }
        });
    } catch (const std::exception& error) {
        unavailable = error.what();
    }
    if (failure)
        std::rethrow_exception(failure);
    if (!result)
        throw detail::refusal(code::unavailable, "The watch's executor is not available: " + boundedText(unavailable, maxTextBytes - 48));
    co_return std::move(*result);
}

/// What BasicWF::watch passes for a watch without an executor, without a mutator, or that owns no state.
struct NoExecutor { };
struct NoMutator { };
struct NoState { };

namespace detail {
/// Whether an accessor returns a reference to its object, not a copy. A visitor is not an accessor.
template <typename Stored, typename State, bool visitor, bool stateful>
[[nodiscard]] constexpr bool returnsReference() noexcept {
    if constexpr (visitor)
        return false;
    else if constexpr (stateful)
        return std::is_lvalue_reference_v<std::invoke_result_t<Stored&, State&>>;
    else
        return std::is_lvalue_reference_v<std::invoke_result_t<Stored&>>;
}
}  // namespace detail

#ifdef _MSC_VER
   // A source that always throws (a test of a failing source, an application's placeholder) makes the returns of the
    // closure below provably unreachable for that instantiation; MSVC flags it under /WX. It is expected, not a bug.
    #pragma warning(push)
    #pragma warning(disable : 4702)
#endif
/// Builds a watch from its source, and optionally the state it owns, its executor and its mutator (BasicWF::watch).
template <typename Shape, typename Source, typename State, typename Executor, typename Mutator>
[[nodiscard]] std::shared_ptr<detail::WatchCore> makeWatch(std::string               name,
                                                           std::string               location,
                                                           Source&&                  source,
                                                           std::shared_ptr<State>    owner,
                                                           [[maybe_unused]] Executor executor,
                                                           [[maybe_unused]] Mutator  mutator) {
    using Stored               = std::decay_t<Source>;  // a function given by name is kept as a pointer
    using Object               = typename Shape::Object;
    constexpr bool stateful    = requires { requires Shape::stateful; };
    constexpr bool hasExecutor = !std::same_as<Executor, NoExecutor>;
    // An accessor writes only through a non-constant reference, on the executor that owns the object.
    constexpr bool writable = !std::is_const_v<Object>
                              && (Shape::visitor || (detail::returnsReference<Stored, State, Shape::visitor, stateful>() && hasExecutor));
    // The object is read as what it is: a source of non-constant elements keeps the operations that only a non-constant
    // object offers (a PagedSource's visit). Whether a value may be written is told by access.writable alone.
    using Root = Object;

    auto core             = std::make_shared<detail::WatchCore>();
    core->name            = std::move(name);
    core->kind            = std::string(kindOf<Object>());
    core->location        = std::move(location);
    core->owned           = !std::same_as<State, NoState>;
    core->access.writable = writable;
    auto kept             = std::make_shared<Stored>(std::forward<Source>(source));
    core->access.open     = [kept, owner](detail::FunctionRef<void(const detail::NodeRef&)> use) {
        static_cast<void>(owner);  // kept alive by the watch even when the source does not receive it
        if constexpr (Shape::visitor) {
            bool          visited = false;
            Visit<Object> visit{[&](Object& object) {
                visited = true;
                use(detail::refOf<Root>(object));
            }};
            if constexpr (stateful)
                (*kept)(*owner, std::move(visit));
            else
                (*kept)(std::move(visit));
            return visited;
        } else {
            if constexpr (stateful) {
                decltype(auto) object = (*kept)(*owner);
                use(detail::refOf<Root>(object));
            } else {
                decltype(auto) object = (*kept)();
                use(detail::refOf<Root>(object));
            }
            return true;
        }
    };
    if constexpr (hasExecutor)
        core->hop = [bound = std::move(executor)](std::function<void()> work) {
            return detail::hopOn(bound, std::move(work));
        };
    if constexpr (!std::same_as<Mutator, NoMutator>) {
        static_assert(writable, "A watch's mutator needs a source that writes: a Visit<T> of a non-constant T, or a non-constant reference on an executor");
        auto mutate         = std::make_shared<Mutator>(std::move(mutator));
        core->access.mutate = [mutate](void* root, std::string_view path, std::string_view text) {
            (*mutate)(*static_cast<Object*>(root), path, text);
        };
    }
    return core;
}
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

/**
 * @brief The handle of a watch, as UISubscription is the handle of a subscription: the watch exists while it lives, and
 * the handle stays safe after the application is destroyed. An empty handle registered nothing: outside the developer
 * environment, and in a distribution build, `app.watch` returns one (ADR-027, section 1). Move-only.
 *
 * Unlike UISubscription, destroying the handle of a watch whose source refers to objects of the application waits for an
 * operation in progress on another thread: destroy it outside every lock the source takes, and outside the watch's
 * executor. A watch declared with the state it owns (`watch(name, std::shared_ptr<State>, source)`) never waits.
 */
class Watch {
public:
    Watch() = default;
    Watch(std::weak_ptr<WatchRegistry> owner, std::uint64_t identifier) : registry(std::move(owner)), watchId(identifier) { }
    Watch(const Watch&)            = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&& other) noexcept : registry(std::move(other.registry)), watchId(std::exchange(other.watchId, 0)) { }
    Watch& operator=(Watch&& other) noexcept {
        // A handle being destroyed takes no new watch: it could not remove it. The source of the assignment keeps it.
        if (this != &other && !destroying) {
            // The previous watch is removed once this handle holds the new one: its source, released then, may assign
            // this handle again, and that assignment is the one kept.
            Watch previous{std::move(*this)};
            registry = std::move(other.registry);
            watchId  = std::exchange(other.watchId, 0);
        }
        return *this;
    }
    ~Watch() {
        destroying = true;  // a source released by reset() may assign this handle: the assignment is refused
        reset();
    }

    /// Removes the watch now; the handle becomes empty. The handle is emptied before the removal and not touched after it:
    /// the source, released by the removal, may assign this handle to a new watch, which stays registered.
    void reset() noexcept {
        const auto id    = std::exchange(watchId, 0);
        const auto owner = std::exchange(registry, {}).lock();
        if (id != 0 && owner)
            owner->remove(id);
    }

    /// The identifier the inspectors know the watch by, 0 for an empty handle.
    [[nodiscard]] std::uint64_t id() const noexcept {
        return watchId;
    }

    explicit operator bool() const noexcept {
        return watchId != 0;
    }

private:
    std::weak_ptr<WatchRegistry> registry;
    std::uint64_t                watchId    = 0;
    bool                         destroying = false;
};

}  // namespace webfront::inspect
