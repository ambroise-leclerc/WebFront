/// @brief A simulated storage held in memory: the backend of the Catch2 tests of the services, and the model of a storage
/// without links (FatFs, LittleFS), where the syntax of SafePath is the whole confinement.
#pragma once

#include "Storage.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront::fs {

/**
 * @brief A storage whose entries live in memory, thread-safe and without links.
 *
 * Its content is lost with the last copy of the storage (copies share it) and with the writers still open on it.
 * setAccess() simulates the rights a system would refuse.
 */
class MemoryStorage {
    struct Node {
        EntryType                                type{EntryType::directory};
        std::shared_ptr<const std::string>       content;
        std::map<std::string, Node, std::less<>> children;
        std::chrono::system_clock::time_point    modified{std::chrono::system_clock::now()};
        bool                                     readable{true};
        bool                                     writable{true};
    };

    struct State {
        std::mutex mutex;
        Node       root;
    };

    // The bytes a File reads: shared with the node, so a later replacement does not change an opened file.
    class Content : public ByteSource {
    public:
        explicit Content(std::shared_ptr<const std::string> shared) : bytes(std::move(shared)) { }
        std::size_t read(std::span<char> buffer) override {
            const auto count = std::min(buffer.size(), bytes->size() - position);
            std::copy_n(bytes->data() + position, count, buffer.data());
            position += count;
            return count;
        }
        [[nodiscard]] std::optional<std::uint64_t> size() const override {
            return bytes->size();
        }
        [[nodiscard]] bool seek(std::uint64_t offset) override {
            position = static_cast<std::size_t>(std::min<std::uint64_t>(offset, bytes->size()));
            return true;
        }

    private:
        std::shared_ptr<const std::string> bytes;
        std::size_t                        position{};
    };

public:
    MemoryStorage() : state(std::make_shared<State>()) { }

    /// A write in progress, buffered in memory until commit().
    class Writer {
    public:
        void write(std::span<const char> bytes) {
            if (done)
                throw std::logic_error("The write is already committed");
            buffer.append(bytes.data(), bytes.size());
        }

        void commit() {
            if (done)
                throw std::logic_error("The write is already committed");
            std::scoped_lock lock(state->mutex);
            auto&            parent = directory(state->root, components, components.size() - 1, true);
            const auto&      name   = components.back();
            if (auto existing = parent.children.find(name); existing != parent.children.end()) {
                if (mode == WriteMode::createNew)
                    throw StorageError(StorageErrc::alreadyExists, "'" + detail::joinComponents(components) + "' already exists");
                if (existing->second.type == EntryType::directory)
                    throw StorageError(StorageErrc::isDirectory, "'" + detail::joinComponents(components) + "' is a directory");
                if (!existing->second.writable)
                    throw StorageError(StorageErrc::accessDenied, "'" + detail::joinComponents(components) + "' is read-only");
            }
            Node file;
            file.type    = EntryType::file;
            file.content = std::make_shared<const std::string>(std::move(buffer));
            parent.children.insert_or_assign(name, std::move(file));
            done = true;
        }

    private:
        friend class MemoryStorage;
        Writer(std::shared_ptr<State> storageState, std::vector<std::string> path, WriteMode writeMode)
            : state(std::move(storageState)), components(std::move(path)), mode(writeMode) { }

        std::shared_ptr<State>   state;
        std::vector<std::string> components;
        WriteMode                mode;
        std::string              buffer;
        bool                     done{false};
    };

    DirectoryPage list(std::string_view path, std::string_view after = {}, std::size_t limit = maxStoragePageSize) {
        const auto             components = detail::storageComponents(path);
        std::vector<EntryInfo> entries;
        {
            std::scoped_lock lock(state->mutex);
            const auto&      node = directory(state->root, components, components.size(), false);
            if (!node.readable)
                throw StorageError(StorageErrc::accessDenied, "Cannot list '" + std::string(path) + "'");
            for (const auto& [name, child] : node.children)
                entries.push_back(info(name, child));
        }
        return detail::pageOf(std::move(entries), after, limit);
    }

    EntryInfo status(std::string_view path) {
        const auto       components = detail::storageComponents(path);
        std::scoped_lock lock(state->mutex);
        if (components.empty())
            return info({}, state->root);
        return info(components.back(), entry(components));
    }

    File openRead(std::string_view path) {
        const auto       components = detail::entryComponents(path);
        std::scoped_lock lock(state->mutex);
        const auto&      node = entry(components);
        if (node.type == EntryType::directory)
            throw StorageError(StorageErrc::isDirectory, "'" + std::string(path) + "' is a directory");
        if (!node.readable)
            throw StorageError(StorageErrc::accessDenied, "Cannot read '" + std::string(path) + "'");
        return File{std::make_unique<Content>(node.content)};
    }

    Writer beginWrite(std::string_view path, WriteMode mode) {
        auto             components = detail::entryComponents(path);
        std::scoped_lock lock(state->mutex);
        directory(state->root, components, components.size() - 1, true);
        return Writer{state, std::move(components), mode};
    }

    void createDirectory(std::string_view path) {
        const auto       components = detail::entryComponents(path);
        std::scoped_lock lock(state->mutex);
        auto&            parent = directory(state->root, components, components.size() - 1, true);
        if (parent.children.contains(components.back()))
            throw StorageError(StorageErrc::alreadyExists, "'" + std::string(path) + "' already exists");
        parent.children.emplace(components.back(), Node{});
    }

    void remove(std::string_view path) {
        const auto       components = detail::entryComponents(path);
        std::scoped_lock lock(state->mutex);
        auto&            parent = directory(state->root, components, components.size() - 1, true);
        const auto       found  = parent.children.find(components.back());
        if (found == parent.children.end())
            throw StorageError(StorageErrc::notFound, "'" + std::string(path) + "' does not exist");
        if (found->second.type == EntryType::directory && !found->second.children.empty())
            throw StorageError(StorageErrc::notEmpty, "'" + std::string(path) + "' is not empty");
        parent.children.erase(found);
    }

    void rename(std::string_view from, std::string_view to) {
        const auto       source = detail::entryComponents(from);
        const auto       target = detail::entryComponents(to);
        std::scoped_lock lock(state->mutex);
        auto&            sourceParent = directory(state->root, source, source.size() - 1, true);
        const auto       found        = sourceParent.children.find(source.back());
        if (found == sourceParent.children.end())
            throw StorageError(StorageErrc::notFound, "'" + std::string(from) + "' does not exist");
        if (found->second.type == EntryType::directory && std::ranges::equal(source, target | std::views::take(source.size())))
            throw StorageError(StorageErrc::invalidPath, "Cannot move '" + std::string(from) + "' into itself");
        auto& targetParent = directory(state->root, target, target.size() - 1, true);
        if (targetParent.children.contains(target.back()))
            throw StorageError(StorageErrc::alreadyExists, "'" + std::string(to) + "' already exists");
        auto node = std::move(found->second);
        sourceParent.children.erase(found);
        targetParent.children.emplace(target.back(), std::move(node));
    }

    /// Simulates the rights the system grants on an entry: an unreadable directory cannot be listed nor traversed, an
    /// unwritable directory refuses entries to be created, removed or renamed in it, an unwritable file refuses to be
    /// replaced, an unreadable file to be opened.
    void setAccess(std::string_view path, bool readable, bool writable) {
        const auto       components = detail::storageComponents(path);
        std::scoped_lock lock(state->mutex);
        auto&            node = components.empty() ? state->root : entry(components);
        node.readable         = readable;
        node.writable         = writable;
    }

private:
    std::shared_ptr<State> state;

    static EntryInfo info(std::string name, const Node& node) {
        return {std::move(name), node.type, node.content ? node.content->size() : 0, node.modified, node.readable, node.writable};
    }

    // The directory made of the first `count` components, traversable, and writable when `forChange` is set.
    static Node& directory(Node& root, const std::vector<std::string>& components, std::size_t count, bool forChange) {
        Node* node = &root;
        for (std::size_t index = 0; index < count; ++index) {
            if (!node->readable)
                throw StorageError(StorageErrc::accessDenied, "Cannot traverse '" + prefix(components, index) + "'");
            const auto found = node->children.find(components[index]);
            if (found == node->children.end())
                throw StorageError(StorageErrc::notFound, "'" + prefix(components, index + 1) + "' does not exist");
            if (found->second.type != EntryType::directory)
                throw StorageError(StorageErrc::notDirectory, "'" + prefix(components, index + 1) + "' is not a directory");
            node = &found->second;
        }
        if (!node->readable || (forChange && !node->writable))
            throw StorageError(StorageErrc::accessDenied, "Access to '" + prefix(components, count) + "' is denied");
        return *node;
    }

    Node& entry(const std::vector<std::string>& components) {
        auto&      parent = directory(state->root, components, components.size() - 1, false);
        const auto found  = parent.children.find(components.back());
        if (found == parent.children.end())
            throw StorageError(StorageErrc::notFound, "'" + detail::joinComponents(components) + "' does not exist");
        return found->second;
    }

    static std::string prefix(const std::vector<std::string>& components, std::size_t count) {
        return detail::joinComponents({components.begin(), components.begin() + static_cast<std::ptrdiff_t>(count)});
    }
};

static_assert(Storage<MemoryStorage>);

}  // namespace webfront::fs
