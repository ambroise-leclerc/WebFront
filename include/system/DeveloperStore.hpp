/// @brief Explicit development project access and staged publication, without shell access.
#pragma once

#include "ToolStore.hpp"

#include <atomic>
#include <chrono>
#include <limits>

namespace webfront::developer {

inline constexpr std::size_t   chunkSize           = 256 * 1024;
inline constexpr std::uint64_t maxTransactionBytes = 256 * 1024 * 1024;
inline constexpr std::size_t   maxTransactions     = 8;
/// A transaction untouched for this long is abandoned: its browser reloaded or disconnected.
inline constexpr std::chrono::minutes defaultAbandonedAfter{10};
/// Staging left by a crashed process is removed at startup once it is this old.
inline constexpr std::chrono::hours staleStagingAfter{1};

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
    explicit Store(std::filesystem::path project, std::chrono::steady_clock::duration abandoned = defaultAbandonedAfter)
        : root(std::filesystem::canonical(project)), abandonedAfter(abandoned) {
        if (!std::filesystem::is_directory(root))
            throw std::invalid_argument("Developer project must be a directory");
        removeStaleStaging();
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
        std::ifstream    file   = openForReading(source, path, offset);
        std::vector<std::uint8_t> bytes(chunkSize);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.bad())
            throw std::runtime_error("Cannot read project file: " + path);
        bytes.resize(static_cast<std::size_t>(file.gcount()));
        return bytes;
    }

    std::string begin(std::string destination, bool directory, bool replace = true) {
        std::scoped_lock lock(mutex);
        expireAbandoned();
        if (transactions.size() >= maxTransactions)
            throw std::runtime_error("Too many developer transactions");
        const auto target = projectPath(root, destination);
        if (!replace && std::filesystem::exists(std::filesystem::symlink_status(target)))
            throw std::runtime_error("Starter destination already exists");
        const auto stagingRoot = projectPath(root, ".webfront/developer/staging");
        std::filesystem::create_directories(stagingRoot);
        // Compare resolved paths: a symlink or a differently cased spelling must not reach the staging area.
        const auto resolved = std::filesystem::weakly_canonical(target);
        requireFreeDestination(resolved, std::filesystem::weakly_canonical(stagingRoot));
        const auto [id, staging] = createStaging(stagingRoot);
        transactions.emplace(id, Transaction{staging, target, resolved, std::move(destination), directory, replace, 0, {}, std::chrono::steady_clock::now()});
        return id;
    }

    void write(std::string id, std::string path, std::uint64_t offset, std::vector<std::uint8_t> bytes) {
        std::scoped_lock lock(mutex);
        auto&            tx       = transaction(id);
        const auto       relative = checkedChunk(tx, path, bytes.size());
        auto&            size     = tx.files[relative.generic_string()];
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
        auto&            tx = transaction(id);
        // Revalidate after the browser has finished writing.
        if (projectPath(root, tx.destination) != tx.target)
            throw std::runtime_error("Project path changed");
        const auto source = tx.directory ? tx.staging : tx.staging / "output";
        if (!tx.directory && !std::filesystem::is_regular_file(source))
            throw std::runtime_error("No output was staged");
        const auto backup = tx.staging.parent_path() / (id + "-backup");
        replaceTarget(tx, source, backup);
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
    struct Transaction {
        std::filesystem::path                staging;
        std::filesystem::path                target;
        std::filesystem::path                resolved;
        std::string                          destination;
        bool                                 directory;
        bool                                 replace;
        std::uint64_t                        size;
        std::map<std::string, std::uint64_t> files;
        std::chrono::steady_clock::time_point lastUse;
    };
    Transaction& transaction(const std::string& id) {
        const auto found = transactions.find(id);
        if (found == transactions.end())
            throw std::runtime_error("Unknown or abandoned developer transaction");
        found->second.lastUse = std::chrono::steady_clock::now();
        return found->second;
    }

    void expireAbandoned() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = transactions.begin(); it != transactions.end();) {
            if (now - it->second.lastUse >= abandonedAfter) {
                std::error_code ignored;
                std::filesystem::remove_all(it->second.staging, ignored);
                it = transactions.erase(it);
            } else
                ++it;
        }
    }

    void removeStaleStaging() const {
        std::error_code error;
        const auto      stagingRoot = root / ".webfront" / "developer" / "staging";
        const auto      now         = std::filesystem::file_time_type::clock::now();
        for (const auto& entry : std::filesystem::directory_iterator(stagingRoot, error))
            if (const auto written = entry.last_write_time(error); !error && now - written >= staleStagingAfter)
                std::filesystem::remove_all(entry.path(), error);
    }

    static std::ifstream openForReading(const std::filesystem::path& source, const std::string& path, std::uint64_t offset) {
        const std::string failure = "Cannot read project file: " + path;
        if (!std::filesystem::is_regular_file(source))
            throw std::runtime_error(failure);
        std::ifstream file(source, std::ios::binary);
        if (!file)
            throw std::runtime_error(failure);
        seek(file, offset, "Cannot seek project file: " + path);
        return file;
    }

    static void seek(std::ifstream& file, std::uint64_t offset, const std::string& failure) {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            throw std::invalid_argument("Invalid read offset");
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file)
            throw std::runtime_error(failure);
    }

    // The staging area and the destinations of unfinished transactions are off limits.
    void requireFreeDestination(const std::filesystem::path& resolved, const std::filesystem::path& staging) const {
        if (resolved == root || related(resolved, staging))
            throw std::invalid_argument("Invalid publication destination");
        for (const auto& entry : transactions)
            if (related(entry.second.resolved, resolved))
                throw std::runtime_error("Destination is already being written");
    }

    static std::pair<std::string, std::filesystem::path> createStaging(const std::filesystem::path& stagingRoot) {
        for (;;) {
            auto id        = std::to_string(++nextId);
            auto directory = stagingRoot / id;
            if (std::filesystem::create_directory(directory))
                return {std::move(id), std::move(directory)};
        }
    }

    static std::filesystem::path checkedChunk(const Transaction& tx, const std::string& path, std::size_t size) {
        if (!tx.directory && path != "output")
            throw std::invalid_argument("Expected output file");
        auto relative = fs::tools::safeRelativePath(path);
        if (size > chunkSize || tx.size + size > maxTransactionBytes)
            throw std::length_error("Developer transaction exceeds its byte limit");
        return relative;
    }

    // Moves the staged files over the target, restoring the previous content if that fails.
    static void replaceTarget(const Transaction& tx, const std::filesystem::path& source, const std::filesystem::path& backup) {
        std::filesystem::create_directories(tx.target.parent_path());
        // A backup left by a crashed process must not make the rename below fail.
        std::filesystem::remove_all(backup);
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
    }

    static bool related(const std::filesystem::path& first, const std::filesystem::path& second) {
        return isParent(first, second) || isParent(second, first);
    }

    static bool isParent(const std::filesystem::path& parent, const std::filesystem::path& child) {
        auto part = child.begin();
        for (const auto& component : parent) {
            if (part == child.end() || *part != component)
                return false;
            ++part;
        }
        return true;
    }

    std::filesystem::path                    root;
    std::chrono::steady_clock::duration      abandonedAfter;
    mutable std::mutex                       mutex;
    std::map<std::string, Transaction>       transactions;
    static inline std::atomic<std::uint64_t> nextId{0};
};

}  // namespace webfront::developer
