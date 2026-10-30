/// @brief Per-connection DOM handles, batched mutations and explicit asynchronous reads.
#pragma once
#include "../Task.hpp"
#include "../weblink/DomBatch.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::dom {
class Node;
template <typename UI>
class Document;
struct EventOptions {
    bool preventDefault{};
    bool stopPropagation{};
};
enum class Read : std::uint32_t { text, value, checked, width, height };

namespace detail {
struct Entry {
    bool                    text{}, island{};
    std::uint32_t           parent{};
    std::set<std::uint32_t> children;
};

inline std::uint32_t eventFlags(EventOptions options) noexcept {
    return std::uint32_t(options.preventDefault) | (std::uint32_t(options.stopPropagation) << 1);
}

/// One started co_await of a DOM event. The event, an invalidation and a stop request race for its single completion.
struct EventWaiter {
    virtual ~EventWaiter()                           = default;
    virtual void deliver(std::string value) noexcept = 0;
    virtual void stop() noexcept                     = 0;
    virtual bool done() noexcept                     = 0;
};
using Subscription = std::pair<std::uint32_t, std::uint32_t>;
using Waiters      = std::map<Subscription, std::weak_ptr<EventWaiter>>;

inline bool sameWaiter(const std::weak_ptr<EventWaiter>& a, const std::weak_ptr<EventWaiter>& b) noexcept {
    return !a.owner_before(b) && !b.owner_before(a);
}

inline void stopAll(Waiters& waiters) noexcept {
    for (auto& [key, weak] : waiters)
        if (auto waiter = weak.lock())
            waiter->stop();
}

/// Internal state is shared by all document wrappers for one connection. Mutations are server-thread confined.
struct State : std::enable_shared_from_this<State> {
    std::atomic<bool>                                                                   alive{true};
    std::thread::id                                                                     thread{std::this_thread::get_id()};
    bool                                                                                ready{}, scheduled{};
    std::uint32_t                                                                       nextNode{1}, batchId{1};
    msg::DomBatch                                                                       pending{1};
    std::size_t                                                                         operations{}, nameBytes{};
    std::map<std::string, std::uint32_t, std::less<>>                                   names;
    std::map<std::uint32_t, Entry>                                                      nodes;
    std::map<std::string, std::uint32_t, std::less<>>                                   islands;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::function<void(std::string)>> listeners;
    Waiters                                                                             waiters;
    std::set<Subscription>                                                              unsubscriptions;  // did not fit a full batch
    std::function<void(const msg::DomBatch&)>                                           send;
    std::function<void()>                                                               schedule;
    std::function<void(std::uint32_t, std::string)>                                     onError;
    // Waits stopped off the server thread; post is thread-safe and runs sweep() on the server thread.
    std::mutex                                                       releaseMutex;
    std::vector<std::pair<Subscription, std::weak_ptr<EventWaiter>>> releases;
    std::function<void()>                                            post;

    bool validOnCurrentThread() const noexcept {
        return alive.load() && thread == std::this_thread::get_id();
    }
    void check() const {
        if (!alive)
            throw std::runtime_error("DOM document is closed");
        if (thread != std::this_thread::get_id())
            throw std::logic_error("DOM document requires the server thread");
    }
    Entry& get(std::uint32_t id) {
        check();
        const auto found = nodes.find(id);
        if (found == nodes.end())
            throw std::runtime_error("DOM node is invalid");
        return found->second;
    }
    void wake() {
        if (ready && operations && !scheduled && schedule) {
            scheduled = true;
            schedule();
        }
    }
    void add(msg::DomOperation op) {
        check();
        pending.add(op);
        ++operations;
        wake();
    }
    std::uint32_t intern(std::string_view name) {
        check();
        if (auto found = names.find(name); found != names.end())
            return found->second;
        if (names.size() >= 65536 || name.size() > 1024 || name.size() > 1024 * 1024 - nameBytes)
            throw std::length_error("DOM name dictionary budget exceeded");
        const auto index = static_cast<std::uint32_t>(names.size());
        add({msg::DomOp::name, index, 0, 0, std::string(name)});
        names.emplace(name, index);
        nameBytes += name.size();
        return index;
    }
    std::uint32_t fresh(msg::DomOperation op, Entry entry) {
        check();
        if (nodes.size() >= 65536 || nextNode == 0)
            throw std::length_error("DOM node budget exhausted");
        op.a = nextNode;
        add(std::move(op));
        nodes.emplace(nextNode, std::move(entry));
        return nextNode++;
    }
    void forget(std::uint32_t id) {
        std::vector<std::uint32_t> stack{id};
        Waiters                    stopped;
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            const auto found = nodes.find(current);
            if (found == nodes.end())
                continue;
            for (auto child : found->second.children)
                stack.push_back(child);
            auto listener = listeners.lower_bound({current, 0});
            while (listener != listeners.end() && listener->first.first == current)
                listener = listeners.erase(listener);
            auto waiter = waiters.lower_bound({current, 0});
            while (waiter != waiters.end() && waiter->first.first == current) {
                stopped.insert(*waiter);
                waiter = waiters.erase(waiter);
            }
            nodes.erase(found);
        }
        // A completion may run inline: it observes the subtree already forgotten.
        stopAll(stopped);
    }
    void revoke(const std::string& name) {
        check();
        if (const auto found = islands.find(name); found != islands.end()) {
            // Already queued operations may refer to the revoked subtree: fail closed rather than send a partial batch.
            if (operations) {
                reject(batchId, "DOM island revoked with pending writes");
                return;
            }
            const auto root = found->second;
            islands.erase(found);
            forget(root);
        }
    }
    void close() {
        alive = false;
        {
            std::lock_guard lock(releaseMutex);
            releases.clear();
            post = {};
        }
        nodes.clear();
        listeners.clear();
        islands.clear();
        unsubscriptions.clear();
        auto stopped = std::move(waiters);
        waiters.clear();
        send     = {};
        schedule = {};
        onError  = {};
        stopAll(stopped);
    }
    void reject(std::uint32_t id, std::string reason) {
        auto handler = onError;
        close();
        if (handler)
            handler(id, std::move(reason));
    }
    void flush() {
        check();
        if (!ready || !operations)
            return;
        try {
            send(pending);
        } catch (...) {
            reject(batchId, "DOM transport failed");
            throw;
        }
        operations = 0;
        if (batchId == std::numeric_limits<std::uint32_t>::max()) {
            reject(batchId, "DOM batch identifiers exhausted");
            throw std::length_error("DOM batch identifiers exhausted");
        }
        pending = msg::DomBatch(++batchId);
        for (const auto& key : std::exchange(unsubscriptions, {}))
            unsubscribe(key);
    }
    /// Queues the browser unsubscription, or the first one of the next batch when this one is full.
    void unsubscribe(Subscription key) {
        try {
            add({msg::DomOp::unlisten, key.first, key.second});
        } catch (const std::length_error&) {
            unsubscriptions.insert(key);
        }
    }
    /// Whether a started wait for this subscription has not completed yet. A completed one is only an obsolete entry
    /// until its release reaches the server thread; a new subscription replaces its browser listener.
    bool awaited(Subscription key) {
        const auto found = waiters.find(key);
        if (found == waiters.end())
            return false;
        if (auto waiter = found->second.lock(); waiter && !waiter->done())
            return true;
        waiters.erase(found);
        return false;
    }
    /// Subscribes one wait and returns its event name index. When this throws, nothing is subscribed; a new event
    /// name may stay interned.
    std::uint32_t await(std::uint32_t node, std::string_view event, EventOptions options, std::weak_ptr<EventWaiter> waiter) {
        const auto& entry = get(node);
        if (entry.text || entry.island)
            throw std::invalid_argument("Operation requires a C++-owned DOM element");
        if (const auto found = names.find(event); found != names.end()) {
            if (listeners.contains({node, found->second}))
                throw std::logic_error("DOM event already has a listener");
            if (awaited({node, found->second}))
                throw std::logic_error("DOM event is already awaited");
        }
        const Subscription key{node, intern(event)};
        waiters.emplace(key, std::move(waiter));
        try {
            add({msg::DomOp::listen, node, key.second, eventFlags(options)});
        } catch (...) {
            waiters.erase(key);
            throw;
        }
        unsubscriptions.erase(key);
        return key.second;
    }
    /// Removes the subscription of a wait that ended without its event, unless another subscription replaced it.
    void drop(Subscription key, const std::weak_ptr<EventWaiter>& waiter) {
        if (!validOnCurrentThread())
            return;
        const auto found = waiters.find(key);
        if (found == waiters.end() || !sameWaiter(found->second, waiter))
            return;
        waiters.erase(found);
        unsubscribe(key);
    }
    /// Called from any thread by a wait that ended without its event.
    void release(Subscription key, std::weak_ptr<EventWaiter> waiter) noexcept {
        try {
            if (validOnCurrentThread()) {
                drop(key, waiter);
                return;
            }
            std::lock_guard lock(releaseMutex);
            if (!post)
                return;  // closed, or no server context: nothing remains to release
            releases.emplace_back(key, std::move(waiter));
            // Only the first pending release posts a sweep. A post refused by a shutting-down application leaves
            // them queued, which is harmless: shutdown closes the document and clears them.
            if (releases.size() == 1) {
                try {
                    post();
                } catch (...) {
                    releases.clear();  // a later release posts again
                    throw;
                }
            }
        } catch (const std::exception& error) {
            if (validOnCurrentThread())
                reject(batchId, error.what());
        } catch (...) {
            if (validOnCurrentThread())
                reject(batchId, "unknown error releasing a DOM event wait");
        }
    }
    /// Runs the releases requested off the server thread.
    void sweep() {
        check();
        std::vector<std::pair<Subscription, std::weak_ptr<EventWaiter>>> released;
        {
            std::lock_guard lock(releaseMutex);
            released.swap(releases);
        }
        for (const auto& [key, waiter] : released)
            drop(key, waiter);
    }
    /// Ends the pending wait of this subscription, if any; the caller has already unsubscribed it.
    void stopWaiter(Subscription key) {
        const auto found = waiters.find(key);
        if (found == waiters.end())
            return;
        const auto waiter = found->second.lock();
        waiters.erase(found);
        if (waiter)
            waiter->stop();
    }
    void event(std::uint32_t node, std::uint32_t name, std::string value) {
        check();
        if (auto found = listeners.find({node, name}); found != listeners.end()) {
            auto callback = found->second;  // callback may remove itself or its node
            callback(std::move(value));
        } else if (auto waiting = waiters.find({node, name}); waiting != waiters.end()) {
            auto waiter = waiting->second.lock();
            waiters.erase(waiting);
            unsubscribe({node, name});
            if (waiter)
                waiter->deliver(std::move(value));  // a stopped wait ignores it
        }
    }
};

template <typename Receiver>
class EventOperationState : public EventWaiter, public std::enable_shared_from_this<EventOperationState<Receiver>> {
    using Token = networking::ex::stop_token_of_t<networking::ex::env_of_t<Receiver>>;
    struct OnStop {
        std::weak_ptr<EventOperationState> state;
        void                               operator()() noexcept {
            if (auto shared = state.lock())
                shared->stop();
        }
    };
    enum class Outcome { pending, value, error, stopped };
    Receiver                                                          receiver;
    std::weak_ptr<State>                                              owner;
    std::uint32_t                                                     node;
    std::string                                                       event;
    EventOptions                                                      options;
    std::mutex                                                        mutex;
    bool                                                              starting{true};
    Outcome                                                           outcome{Outcome::pending};
    std::string                                                       value;
    std::exception_ptr                                                error;
    std::optional<std::uint32_t>                                      nameIndex;  // once subscribed
    std::optional<networking::ex::stop_callback_for_t<Token, OnStop>> stopCallback;

    bool finish(Outcome completion, std::string result = {}, std::exception_ptr failure = {}) noexcept {
        bool ready{};
        {
            std::lock_guard lock(mutex);
            if (outcome != Outcome::pending)
                return false;
            outcome = completion;
            value   = std::move(result);
            error   = std::move(failure);
            ready   = !starting;
        }
        if (ready)
            complete();
        return true;
    }
    void complete() noexcept {
        // Exactly one completion won under the mutex; a concurrent stop callback may still be waiting for it.
        stopCallback.reset();
        switch (outcome) {
            case Outcome::value:
                networking::ex::set_value(std::move(receiver), std::move(value));
                break;
            case Outcome::error:
                networking::ex::set_error(std::move(receiver), std::move(error));
                break;
            case Outcome::stopped:
                networking::ex::set_stopped(std::move(receiver));
                break;
            case Outcome::pending:
                std::terminate();
        }
    }
    void subscribe() {
        {
            std::lock_guard lock(mutex);
            if (outcome != Outcome::pending)
                return;  // stopped before start: no subscription
        }
        const auto state = owner.lock();
        if (!state)
            throw std::runtime_error("DOM document is gone");
        const auto index = state->await(node, event, options, this->weak_from_this());
        bool       ended{};
        {
            std::lock_guard lock(mutex);
            nameIndex = index;
            ended = outcome != Outcome::pending;
        }
        if (ended)
            state->drop({node, index}, this->weak_from_this());  // stopped from another thread while subscribing
    }

public:
    EventOperationState(Receiver r, std::weak_ptr<State> document, std::uint32_t id, std::string name, EventOptions flags)
        : receiver(std::move(r)), owner(std::move(document)), node(id), event(std::move(name)), options(flags) { }
    void start() noexcept {
        // Completion is deferred until subscription is finished: registering a stopped token invokes OnStop inline.
        try {
            stopCallback.emplace(networking::ex::get_stop_token(networking::ex::get_env(receiver)), OnStop{this->weak_from_this()});
            subscribe();
        } catch (...) {
            finish(Outcome::error, {}, std::current_exception());
        }
        bool ready{};
        {
            std::lock_guard lock(mutex);
            starting = false;
            ready    = outcome != Outcome::pending;
        }
        if (ready)
            complete();
    }
    void deliver(std::string result) noexcept override {
        finish(Outcome::value, std::move(result));
    }
    void stop() noexcept override {
        if (!finish(Outcome::stopped))
            return;
        std::optional<std::uint32_t> subscribed;
        {
            std::lock_guard lock(mutex);
            subscribed = nameIndex;
        }
        // A stop from the document has already removed the subscription; one from the stop token releases it.
        if (const auto state = owner.lock(); state && subscribed)
            state->release({node, *subscribed}, this->weak_from_this());
    }
    bool done() noexcept override {
        std::lock_guard lock(mutex);
        return outcome != Outcome::pending;
    }
};

/// Completes with the value of the next event delivered to C++ after start, or stopped when it cannot occur any more.
class EventSender {
    std::weak_ptr<State> owner;
    std::uint32_t        node;
    std::string          event;
    EventOptions         options;

    template <typename Receiver>
    class Operation {
        std::shared_ptr<EventOperationState<Receiver>> state;

    public:
        using operation_state_concept = networking::ex::operation_state_tag;
        explicit Operation(std::shared_ptr<EventOperationState<Receiver>> value) : state(std::move(value)) { }
        Operation(const Operation&)            = delete;
        Operation(Operation&&)                 = delete;
        Operation& operator=(const Operation&) = delete;
        Operation& operator=(Operation&&)      = delete;
        void       start() noexcept {
            auto keepAlive = state;  // receiver completion may destroy this operation inline
            keepAlive->start();
        }
    };

public:
    using sender_concept        = networking::ex::sender_tag;
    using completion_signatures = networking::ex::completion_signatures<networking::ex::set_value_t(std::string), networking::ex::set_error_t(std::exception_ptr),
                                                                        networking::ex::set_stopped_t()>;
    EventSender(std::weak_ptr<State> document, std::uint32_t id, std::string name, EventOptions flags)
        : owner(std::move(document)), node(id), event(std::move(name)), options(flags) { }
    template <typename Receiver>
    auto connect(Receiver receiver) const {
        // Not make_shared: an obsolete weak entry must not keep the receiver's storage allocated.
        return Operation<Receiver>{
            std::shared_ptr<EventOperationState<Receiver>>(new EventOperationState<Receiver>(std::move(receiver), owner, node, event, options))};
    }
};
}  // namespace detail

/** @brief An identifier tied to a document; copying it does not copy a DOM subtree. */
class Node {
    std::weak_ptr<detail::State> owner;
    std::uint32_t                identifier{};
    template <typename UI>
    friend class Document;
    Node(std::shared_ptr<detail::State> state, std::uint32_t id) : owner(std::move(state)), identifier(id) { }
    std::shared_ptr<detail::State> state() const {
        auto shared = owner.lock();
        if (!shared)
            throw std::runtime_error("DOM document is gone");
        shared->get(identifier);
        return shared;
    }
    std::shared_ptr<detail::State> elementState() const {
        auto        shared = state();
        const auto& entry  = shared->get(identifier);
        if (entry.text || entry.island)
            throw std::invalid_argument("Operation requires a C++-owned DOM element");
        return shared;
    }
    void same(const Node& other) const {
        if (owner.lock() != other.owner.lock())
            throw std::invalid_argument("DOM nodes belong to different documents");
        other.state();
    }
    void insert(Node child, std::uint32_t reference) const {
        auto s = state();
        same(child);
        if (s->get(identifier).text || s->get(child.identifier).island)
            throw std::invalid_argument("Invalid DOM parent or island move");
        for (auto ancestor = identifier; ancestor; ancestor = s->get(ancestor).parent)
            if (ancestor == child.identifier)
                throw std::invalid_argument("DOM cycle");
        s->add({reference ? msg::DomOp::insertBefore : msg::DomOp::append, identifier, child.identifier, reference});
        auto& entry = s->get(child.identifier);
        if (entry.parent)
            s->get(entry.parent).children.erase(child.identifier);
        entry.parent = identifier;
        s->get(identifier).children.insert(child.identifier);
    }

public:
    Node() = default;
    std::uint32_t id() const {
        state();
        return identifier;
    }
    /// Whether this handle is live and usable on the calling thread; never touches the node table off-thread.
    bool valid() const noexcept {
        auto s = owner.lock();
        return s && s->validOnCurrentThread() && s->nodes.contains(identifier);
    }
    void setText(std::string value) const {
        auto s = state();
        if (!s->get(identifier).text)
            throw std::invalid_argument("setText requires a text node");
        s->add({msg::DomOp::setText, identifier, 0, 0, std::move(value)});
    }
    void attribute(std::string_view name, std::string value) const {
        auto s = elementState();
        s->add({msg::DomOp::attribute, identifier, s->intern(name), 0, std::move(value)});
    }
    void unsetAttribute(std::string_view name) const {
        auto s = elementState();
        s->add({msg::DomOp::unsetAttribute, identifier, s->intern(name)});
    }
    void property(std::string_view name, std::string value) const {
        auto s = elementState();
        s->add({msg::DomOp::property, identifier, s->intern(name), 0, std::move(value)});
    }
    void append(Node child) const {
        insert(child, 0);
    }
    void insertBefore(Node child, Node reference) const {
        auto s = state();
        same(reference);
        if (s->get(reference.identifier).parent != identifier)
            throw std::invalid_argument("DOM reference is not a child");
        same(child);
        if (child.identifier == reference.identifier)
            return;  // Like native insertBefore(x, x): preserve the subtree without emitting a mutation.
        insert(child, reference.identifier);
    }
    void remove() const {
        auto       s      = state();
        const auto parent = s->get(identifier).parent;
        if (s->get(identifier).island)
            throw std::invalid_argument("Cannot remove a page-owned island");
        s->add({msg::DomOp::remove, identifier});
        if (parent)
            s->get(parent).children.erase(identifier);
        s->forget(identifier);
    }
    void listen(std::string_view event, std::function<void(std::string)> callback, EventOptions options = {}) const {
        auto s = elementState();
        if (!callback)
            throw std::invalid_argument("DOM event callback must not be empty");
        if (const auto found = s->names.find(event); found != s->names.end() && s->awaited({identifier, found->second}))
            throw std::logic_error("DOM event is already awaited");
        const auto name = s->intern(event);
        s->add({msg::DomOp::listen, identifier, name, detail::eventFlags(options)});
        s->listeners[{identifier, name}] = std::move(callback);
        s->unsubscriptions.erase({identifier, name});
    }
    /// Also ends a pending next() for this event, which completes stopped.
    void unlisten(std::string_view event) const {
        auto       s    = elementState();
        const auto name = s->intern(event);
        s->add({msg::DomOp::unlisten, identifier, name});
        s->listeners.erase({identifier, name});
        s->unsubscriptions.erase({identifier, name});
        s->stopWaiter({identifier, name});
    }
    /**
     * @brief A sender of the value of this element's next event, as a listen() callback would receive it.
     *
     * Starting it subscribes on the server thread, with the flags declared now; its event, or the end of that wait,
     * unsubscribes. Events delivered while no wait is started are not queued: use listen() for a stream.
     * It completes stopped through the receiver's stop token (a TaskHandle), unlisten(), removal or revocation of the
     * node, disconnection, rejection and shutdown. A second wait or a listen() callback for the same event is an error.
     */
    [[nodiscard]] detail::EventSender next(std::string_view event, EventOptions options = {}) const {
        elementState();
        return {owner, identifier, std::string(event), options};
    }
    bool belongsTo(const std::shared_ptr<detail::State>& s) const {
        return owner.lock() == s;
    }
};
struct ReadRequest {
    Node node;
    Read property;
};

/** @brief One document per connected browser. All methods run on the server thread. */
template <typename UI>
class Document {
    UI                             ui;
    std::shared_ptr<detail::State> state;

public:
    explicit Document(UI browser) : ui(browser), state(browser.domState()) { }
    /// Whether this document is live and usable on the calling thread.
    bool valid() const noexcept {
        return state && state->validOnCurrentThread();
    }
    Node island(std::string name = "root") const {
        state->check();
        if (auto found = state->islands.find(name); found != state->islands.end())
            return {state, found->second};
        const auto index = state->intern(name);
        const auto id    = state->fresh({msg::DomOp::mount, 0, index}, {.text = false, .island = true, .parent = 0, .children = {}});
        state->islands.emplace(std::move(name), id);
        return {state, id};
    }
    Node element(std::string_view tag) const {
        const auto name = state->intern(tag);
        return {state, state->fresh({msg::DomOp::element, 0, name}, {})};
    }
    Node text(std::string value) const {
        return {state, state->fresh({msg::DomOp::text, 0, 0, 0, std::move(value)}, {.text = true, .island = false, .parent = 0, .children = {}})};
    }
    void flush() const {
        state->flush();
    }
    void onError(std::function<void(std::uint32_t, std::string)> callback) const {
        state->check();
        state->onError = std::move(callback);
    }
    /// Flushes writes immediately, even if the returned sender is discarded. Only the grouped read RPC is lazy and locally cancellable.
    auto read(std::vector<ReadRequest> requests) const {
        state->check();
        if (requests.empty() || requests.size() > 4096)
            throw std::length_error("DOM reads require 1..4096 requests");
        std::vector<std::uint32_t> encoded;
        for (const auto& request : requests) {
            if (!request.node.belongsTo(state))
                throw std::invalid_argument("DOM read belongs to another document");
            if (request.property > Read::height)
                throw std::invalid_argument("Unknown DOM read");
            encoded.push_back(request.node.id());
            encoded.push_back(static_cast<std::uint32_t>(request.property));
        }
        if (!state->ready)
            throw std::runtime_error("DOM interpreter has not connected");
        flush();
        return networking::ex::then(ui.template jsFunctionSender<std::vector<std::uint8_t>>("wfDom.read")(encoded),
                                    [s = state, requests = std::move(requests)](std::vector<std::uint8_t> bytes) {
                                        s->check();
                                        if (bytes.size() > msg::DomBatch::maxBytes)
                                            throw std::length_error("DOM read result exceeds 1 MiB");
                                        std::vector<std::string> result;
                                        std::size_t              offset{};
                                        for (const auto& request : requests) {
                                            request.node.id();  // Revoked handles cannot produce a successful late result.
                                            if (bytes.size() - offset < 4)
                                                throw std::runtime_error("Truncated DOM read result");
                                            std::uint32_t size{};
                                            for (unsigned shift = 0; shift < 32; shift += 8)
                                                size |= std::uint32_t(bytes[offset++]) << shift;
                                            if (size > bytes.size() - offset)
                                                throw std::runtime_error("Truncated DOM read value");
                                            result.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset), size);
                                            msg::DomBatch::validateUtf8(result.back());
                                            offset += size;
                                        }
                                        if (offset != bytes.size())
                                            throw std::runtime_error("Trailing DOM read data");
                                        return result;
                                    });
    }
};
}  // namespace webfront::dom
