/// @brief Explicit development project access and staged publication, without shell access.
#pragma once

#include "DeveloperFiles.hpp"

namespace webfront::developer {

class Store {
public:
    explicit Store(std::filesystem::path project, std::chrono::steady_clock::duration abandoned = defaultAbandonedAfter)
        : root(std::filesystem::canonical(project)), abandonedAfter(abandoned) {
        if (!std::filesystem::is_directory(root))
            throw std::invalid_argument("Developer project must be a directory");
        removeStaleStaging(root);
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
        const auto source = stagedSource(tx);
        const auto backup = tx.staging.parent_path() / (id + "-backup");
        replaceTarget(tx.target, tx.replace, source, backup);
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

    // The staging area and the destinations of unfinished transactions are off limits.
    void requireFreeDestination(const std::filesystem::path& resolved, const std::filesystem::path& staging) const {
        if (resolved == root || related(resolved, staging))
            throw std::invalid_argument("Invalid publication destination");
        requireNoOverlap(resolved);
    }

    void requireNoOverlap(const std::filesystem::path& resolved) const {
        for (const auto& entry : transactions)
            if (related(entry.second.resolved, resolved))
                throw std::runtime_error("Destination is already being written");
    }

    static std::filesystem::path stagedSource(const Transaction& tx) {
        if (tx.directory)
            return tx.staging;
        auto output = tx.staging / "output";
        if (!std::filesystem::is_regular_file(output))
            throw std::runtime_error("No output was staged");
        return output;
    }

    static void requireWithinLimits(const Transaction& tx, std::size_t size) {
        if (size > chunkSize)
            throw std::length_error("Developer transaction exceeds its byte limit");
        if (tx.size + size > maxTransactionBytes)
            throw std::length_error("Developer transaction exceeds its byte limit");
    }

    static std::filesystem::path checkedChunk(const Transaction& tx, const std::string& path, std::size_t size) {
        if (!tx.directory && path != "output")
            throw std::invalid_argument("Expected output file");
        auto relative = fs::tools::safeRelativePath(path);
        requireWithinLimits(tx, size);
        return relative;
    }

    std::filesystem::path                    root;
    std::chrono::steady_clock::duration      abandonedAfter;
    mutable std::mutex                       mutex;
    std::map<std::string, Transaction>       transactions;
};

}  // namespace webfront::developer
