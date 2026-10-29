/// @brief Per-connection DOM handles, batched mutations and explicit asynchronous reads.
#pragma once
#include "../Task.hpp"
#include "../weblink/DomBatch.hpp"

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>

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
    std::function<void(const msg::DomBatch&)>                                           send;
    std::function<void()>                                                               schedule;
    std::function<void(std::uint32_t, std::string)>                                     onError;

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
            nodes.erase(found);
        }
    }
    void revoke(const std::string& name) {
        check();
        if (const auto found = islands.find(name); found != islands.end()) {
            // Already queued operations may refer to the revoked subtree: fail closed rather than send a partial batch.
            if (operations) {
                reject(batchId, "DOM island revoked with pending writes");
                return;
            }
            forget(found->second);
            islands.erase(found);
        }
    }
    void close() {
        alive = false;
        nodes.clear();
        listeners.clear();
        islands.clear();
        send     = {};
        schedule = {};
        onError  = {};
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
    }
    void event(std::uint32_t node, std::uint32_t name, std::string value) {
        check();
        if (auto found = listeners.find({node, name}); found != listeners.end()) {
            auto callback = found->second;  // callback may remove itself or its node
            callback(std::move(value));
        }
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
        const auto name = s->intern(event);
        s->add({msg::DomOp::listen, identifier, name, std::uint32_t(options.preventDefault) | (std::uint32_t(options.stopPropagation) << 1)});
        s->listeners[{identifier, name}] = std::move(callback);
    }
    void unlisten(std::string_view event) const {
        auto       s    = elementState();
        const auto name = s->intern(event);
        s->add({msg::DomOp::unlisten, identifier, name});
        s->listeners.erase({identifier, name});
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
