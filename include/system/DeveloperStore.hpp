/// @brief Explicit development project access and staged publication, without shell access.
#pragma once

#include "ToolStore.hpp"

#include <atomic>
#include <limits>

namespace webfront::developer {

inline constexpr std::size_t   chunkSize           = 256 * 1024;
inline constexpr std::uint64_t maxTransactionBytes = 256 * 1024 * 1024;

// Resolve existing parents too: a symlink must never turn a relative project path into an escape.
inline std::filesystem::path projectPath(const std::filesystem::path& root, std::string_view relative) {
    const auto path     = root / fs::tools::safeRelativePath(relative);
    const auto resolved = std::filesystem::weakly_canonical(path);
    auto       base     = root.begin();
    auto       part     = resolved.begin();
    for (; base != root.end(); ++base, ++part)
        if (part == resolved.end() || *part != *base)
            throw std::invalid_argument("Path escapes the developer project");
    return path;
}

class Store {
public:
    explicit Store(std::filesystem::path project) : root(std::filesystem::canonical(project)) {
        if (!std::filesystem::is_directory(root))
            throw std::invalid_argument("Developer project must be a directory");
    }
    Store(const Store&)            = delete;
    Store& operator=(const Store&) = delete;
    ~Store() {
        for (const auto& [id, transaction] : transactions) {
            (void)id;
            std::error_code ignored;
            std::filesystem::remove_all(transaction.staging, ignored);
        }
    }

    std::vector<std::uint8_t> read(std::string path, std::uint64_t offset) const {
        std::scoped_lock lock(mutex);
        const auto       source = projectPath(root, path);
        if (!std::filesystem::is_regular_file(source))
            throw std::runtime_error("Cannot read project file: " + path);
        std::ifstream file(source, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot read project file: " + path);
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            throw std::invalid_argument("Invalid read offset");
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file)
            throw std::runtime_error("Cannot seek project file: " + path);
        std::vector<std::uint8_t> bytes(chunkSize);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.bad())
            throw std::runtime_error("Cannot read project file: " + path);
        bytes.resize(static_cast<std::size_t>(file.gcount()));
        return bytes;
    }

    std::string begin(std::string destination, bool directory, bool replace = true) {
        std::scoped_lock lock(mutex);
        if (transactions.size() >= 8)
            throw std::runtime_error("Too many developer transactions");
        const auto target = projectPath(root, destination);
        if (!replace && std::filesystem::exists(std::filesystem::symlink_status(target)))
            throw std::runtime_error("Starter destination already exists");
        if (target == root || destination == ".webfront" || destination == ".webfront/developer" || destination.starts_with(".webfront/developer/staging"))
            throw std::invalid_argument("Invalid publication destination");
        for (const auto& [id, tx] : transactions) {
            (void)id;
            if (tx.target == target || isParent(tx.target, target) || isParent(target, tx.target))
                throw std::runtime_error("Destination is already being written");
        }
        const auto stagingRoot = projectPath(root, ".webfront/developer/staging");
        std::filesystem::create_directories(stagingRoot);
        std::filesystem::path staging;
        std::string           id;
        do {
            id      = std::to_string(++nextId);
            staging = stagingRoot / id;
        } while (!std::filesystem::create_directory(staging));
        transactions.emplace(id, Transaction{staging, target, std::move(destination), directory, replace, 0, {}});
        return id;
    }

    void write(std::string id, std::string path, std::uint64_t offset, std::vector<std::uint8_t> bytes) {
        std::scoped_lock lock(mutex);
        auto&            tx = transactions.at(id);
        if (!tx.directory && path != "output")
            throw std::invalid_argument("Expected output file");
        const auto relative = fs::tools::safeRelativePath(path);
        if (bytes.size() > chunkSize || tx.size + bytes.size() > maxTransactionBytes)
            throw std::length_error("Developer transaction exceeds its byte limit");
        const auto key  = relative.generic_string();
        auto&      size = tx.files[key];
        if (offset != size)
            throw std::invalid_argument("Non-sequential developer write");
        const auto target = tx.staging / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary | std::ios::app);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file)
            throw std::runtime_error("Cannot write staged developer file");
        size    += bytes.size();
        tx.size += bytes.size();
    }

    void commit(std::string id) {
        std::scoped_lock lock(mutex);
        auto&            tx = transactions.at(id);
        // Revalidate after the browser has finished writing.
        if (projectPath(root, tx.destination) != tx.target)
            throw std::runtime_error("Project path changed");
        const auto source = tx.directory ? tx.staging : tx.staging / "output";
        if (!tx.directory && !std::filesystem::is_regular_file(source))
            throw std::runtime_error("No output was staged");
        std::filesystem::create_directories(tx.target.parent_path());
        const auto backup = tx.staging.parent_path() / (id + "-backup");
        const bool exists = std::filesystem::exists(std::filesystem::symlink_status(tx.target));
        if (exists && !tx.replace)
            throw std::runtime_error("Starter destination already exists");
        if (exists)
            std::filesystem::rename(tx.target, backup);
        try {
            std::filesystem::rename(source, tx.target);
        } catch (...) {
            if (exists)
                std::filesystem::rename(backup, tx.target);
            throw;
        }
        std::error_code ignored;
        std::filesystem::remove_all(backup, ignored);
        std::filesystem::remove_all(tx.staging, ignored);
        transactions.erase(id);
    }

    void abort(std::string id) {
        std::scoped_lock lock(mutex);
        const auto       found = transactions.find(id);
        if (found == transactions.end())
            return;
        std::filesystem::remove_all(found->second.staging);
        transactions.erase(found);
    }

private:
    static bool isParent(const std::filesystem::path& parent, const std::filesystem::path& child) {
        auto part = child.begin();
        for (const auto& component : parent) {
            if (part == child.end() || *part != component)
                return false;
            ++part;
        }
        return true;
    }

    struct Transaction {
        std::filesystem::path                staging;
        std::filesystem::path                target;
        std::string                          destination;
        bool                                 directory;
        bool                                 replace;
        std::uint64_t                        size;
        std::map<std::string, std::uint64_t> files;
    };
    std::filesystem::path                    root;
    mutable std::mutex                       mutex;
    std::map<std::string, Transaction>       transactions;
    static inline std::atomic<std::uint64_t> nextId{0};
};

}  // namespace webfront::developer
