// ADR-018 (#378): throwaway prototype of a C++ owned DOM subtree, never part of the library. C++ records mutations in a
// batch (a compact binary stream, the model of Dioxus and Remote DOM), flush() sends the whole batch as one Uint8Array
// through the existing bridge, and dom-interpreter.js validates it entirely before applying it. Events come back as calls
// of the C++ callback "wfDom.event". Every access happens on the thread of the bridge callbacks (the server thread).
//
// Wire format of a batch: one version byte (0), then operations. Integers are unsigned LEB128 varints and strings are a
// varint byte length followed by UTF-8. Names (tags, attributes, properties, events, islands) are interned once per
// document with NAME and then referenced by index.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dom_prototype {

enum class Op : std::uint8_t {
    name = 1,        // index, string
    element,         // id, tag
    text,            // id, string
    attribute,       // id, name, string
    unsetAttribute,  // id, name
    property,        // id, name, string
    setText,         // id, string
    append,          // parent, child
    insertBefore,    // parent, child, reference
    remove,          // id (the interpreter forgets the identifiers of the whole subtree)
    listen,          // id, event, flags
    unlisten,        // id, event
    mount,           // id, island: the island element registered by the page becomes the node id
};

// Decided when listening, since an event cannot wait for C++ to choose (the handler runs after a round trip).
enum Flags : std::uint8_t { none = 0, preventDefault = 1, stopPropagation = 2 };

class Batch {
public:
    Batch() {
        bytes.push_back(0);
    }

    void op(Op value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        ++count;
    }
    void varint(std::uint32_t value) {
        while (value >= 0x80) {
            bytes.push_back(static_cast<std::uint8_t>(value | 0x80));
            value >>= 7;
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    void string(std::string_view value) {
        varint(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void byte(std::uint8_t value) {
        bytes.push_back(value);
    }

    [[nodiscard]] bool empty() const {
        return count == 0;
    }
    [[nodiscard]] std::size_t operations() const {
        return count;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() {
        auto result = std::move(bytes);
        bytes       = {0};
        count       = 0;
        return result;
    }

private:
    std::vector<std::uint8_t> bytes;
    std::size_t               count = 0;
};

using Handler = std::function<void(std::string_view value)>;

template <typename UI>
class Document;

// A handle to a node of the C++ owned subtree; cheap to copy, valid while the document lives.
template <typename UI>
class Node {
public:
    Node(Document<UI>& owner, std::uint32_t identifier) : doc(&owner), nodeId(identifier) {}

    [[nodiscard]] std::uint32_t id() const {
        return nodeId;
    }

    Node& attr(std::string_view name, std::string_view value) {
        doc->attribute(*this, name, value);
        return *this;
    }
    Node& prop(std::string_view name, std::string_view value) {
        doc->property(*this, name, value);
        return *this;
    }
    Node& child(Node node) {
        doc->append(*this, node);
        return *this;
    }
    Node& text(std::string_view value) {
        return child(doc->text(value));
    }
    Node& on(std::string_view event, Handler handler, std::uint8_t flags = none) {
        doc->listen(*this, event, std::move(handler), flags);
        return *this;
    }

private:
    Document<UI>* doc;
    std::uint32_t nodeId;
};

template <typename UI>
class Document {
public:
    using NodeType = Node<UI>;

    explicit Document(UI browser) : ui(browser) {}

    NodeType element(std::string_view tag) {
        const auto tagIndex = name(tag);
        const auto id       = nextId++;
        batch.op(Op::element);
        batch.varint(id);
        batch.varint(tagIndex);
        return {*this, id};
    }
    NodeType text(std::string_view value) {
        const auto id = nextId++;
        batch.op(Op::text);
        batch.varint(id);
        batch.string(value);
        return {*this, id};
    }
    // The island the page registered under this name, as a node of this document.
    NodeType island(std::string_view islandName) {
        const auto index = name(islandName);
        const auto id    = nextId++;
        batch.op(Op::mount);
        batch.varint(id);
        batch.varint(index);
        return {*this, id};
    }
    void attribute(NodeType node, std::string_view attributeName, std::string_view value) {
        const auto index = name(attributeName);
        batch.op(Op::attribute);
        batch.varint(node.id());
        batch.varint(index);
        batch.string(value);
    }
    void property(NodeType node, std::string_view propertyName, std::string_view value) {
        const auto index = name(propertyName);
        batch.op(Op::property);
        batch.varint(node.id());
        batch.varint(index);
        batch.string(value);
    }
    void setText(NodeType node, std::string_view value) {
        batch.op(Op::setText);
        batch.varint(node.id());
        batch.string(value);
    }
    void append(NodeType parent, NodeType child) {
        batch.op(Op::append);
        batch.varint(parent.id());
        batch.varint(child.id());
    }
    void remove(NodeType node) {
        batch.op(Op::remove);
        batch.varint(node.id());
        // The prototype does not keep the tree: handlers of removed descendants stay until clearHandlers().
        for (auto it = handlers.lower_bound({node.id(), 0}); it != handlers.end() && it->first.first == node.id();)
            it = handlers.erase(it);
    }
    void listen(NodeType node, std::string_view event, Handler handler, std::uint8_t flags) {
        const auto index = name(event);
        batch.op(Op::listen);
        batch.varint(node.id());
        batch.varint(index);
        batch.byte(flags);
        handlers[{node.id(), index}] = std::move(handler);
    }
    void clearHandlers() {
        handlers.clear();
    }

    // Sends the pending mutations as one call; returns the number of bytes sent (0 when nothing was pending).
    std::size_t flush() {
        if (batch.empty())
            return 0;
        auto       bytes = batch.take();
        const auto size  = bytes.size();
        ui.jsFunction("wfDom.apply")(bytes);
        return size;
    }
    [[nodiscard]] std::size_t pendingOperations() const {
        return batch.operations();
    }

    // Called by the "wfDom.event" callback: an unknown node or event is ignored (it may have been removed meanwhile).
    void dispatch(std::uint32_t id, std::uint32_t event, std::string_view value) {
        if (const auto it = handlers.find({id, event}); it != handlers.end()) {
            auto handler = it->second;  // the handler may remove its own node
            handler(value);
        }
    }

private:
    std::uint32_t name(std::string_view value) {
        if (const auto it = names.find(std::string(value)); it != names.end())
            return it->second;
        const auto index = static_cast<std::uint32_t>(names.size());
        names.emplace(std::string(value), index);
        batch.op(Op::name);
        batch.varint(index);
        batch.string(value);
        return index;
    }

    UI                                                         ui;
    Batch                                                      batch;
    std::uint32_t                                              nextId = 1;
    std::unordered_map<std::string, std::uint32_t>             names;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Handler> handlers;
};

}  // namespace dom_prototype
