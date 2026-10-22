/// @brief Explicit development project access and staged publication, without shell access.
#pragma once

#include "DeveloperFiles.hpp"
#include "DeveloperJournal.hpp"
#include "../utils/Json.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

namespace webfront::developer {

class Store {
public:
    /// Opening a project completes the group publications that reached their commit point before
    /// the previous process stopped and abandons the others. A publication that cannot be completed
    /// does not prevent opening the project: state() and commitGroup() retry it and report why it fails,
    /// and recoveryFailure() describes it meanwhile. The hook is for tests only.
    explicit Store(std::filesystem::path project, std::chrono::steady_clock::duration abandoned = defaultAbandonedAfter,
                   StepHook stepHook = {})
        : root(std::filesystem::canonical(project)), abandonedAfter(abandoned), hook(std::move(stepHook)) {
        if (!std::filesystem::is_directory(root))
            throw std::invalid_argument("Developer project must be a directory");
        removeStaleStaging(root);
        try {
            recoverJournals(root, hook);
        } catch (const IncompletePublication& incomplete) {
            pendingRecovery = incomplete.what();
        }
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

    /// The names in a project directory as a sorted JSON array of strings; a directory's name ends with '/'. An
    /// empty path lists the project root. An entry that a link would take out of the project, or whose type cannot
    /// be read, is omitted; only a directory that cannot be read fails the listing.
    std::vector<std::uint8_t> list(std::string directory) const {
        std::scoped_lock lock(mutex);
        const auto       source = directory.empty() ? root : projectPath(root, directory);
        std::error_code  error;
        if (!std::filesystem::is_directory(source, error))
            throw std::runtime_error("Cannot list project directory: " + directory);
        std::vector<std::string> names;
        std::filesystem::directory_iterator entries(source, error);
        for (; !error && entries != std::filesystem::directory_iterator{}; entries.increment(error)) {
            const auto name = entries->path().filename().generic_string();
            if (auto listed = listedName(directory, name)) {
                if (names.size() == maxListedEntries)
                    throw std::length_error("Project directory has more than 4096 entries: " + directory);
                names.push_back(std::move(*listed));
            }
        }
        if (error)
            throw std::runtime_error("Cannot list project directory: " + directory);
        std::ranges::sort(names);
        std::string text = "[";
        for (const auto& name : names)
            text += (text.size() > 1 ? "," : "") + json::quote(name);
        text += "]";
        return {text.begin(), text.end()};
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
        transactions.emplace(id, Transaction{staging, target, resolved, std::move(destination), directory, replace, 0, {}, {}, std::chrono::steady_clock::now()});
        return id;
    }

    /// The two forms of begin() exposed to the browser: replacing an existing destination or refusing to.
    std::string beginReplacing(std::string destination, bool directory) {
        return begin(std::move(destination), directory);
    }
    std::string beginNew(std::string destination, bool directory) {
        return begin(std::move(destination), directory, false);
    }

    void write(std::string id, std::string path, std::uint64_t offset, std::vector<std::uint8_t> bytes) {
        std::scoped_lock lock(mutex);
        auto&            tx       = transaction(id);
        const auto       relative = checkedChunk(tx, path, bytes.size());
        requireDistinctCase(tx, relative.generic_string());
        auto& size = tx.files[relative.generic_string()];
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

    /// Identifies the current content of project files; see commitGroup(). A committed publication that is
    /// not applied yet is completed first; while it cannot be, this throws IncompletePublication.
    std::string state(const std::vector<std::string>& paths) {
        std::scoped_lock lock(mutex);
        recover();
        return fingerprint(root, paths);
    }

    /// Why a committed publication could not be completed, if it still cannot.
    std::optional<std::string> recoveryFailure() const {
        std::scoped_lock lock(mutex);
        return pendingRecovery;
    }

    /// Publishes several staged files as one change. The change is applied only if the files named by
    /// `paths` (which include every destination) still have the content identified by `expected`, as
    /// read by state() when the operation began; otherwise nothing changes and the caller runs again.
    ///
    /// The commit point is the journal, completely written and synchronized after the files are
    /// staged and before the first replacement. A failure before it returns the files to their
    /// transactions, which the caller may commit again or abort; a crash before it leaves the previous
    /// files. After it, the publication happens: if a replacement fails, this throws
    /// IncompletePublication and the next state(), commitGroup() or opening of the project completes it.
    void commitGroup(const std::vector<std::string>& ids, const std::vector<std::string>& paths, const std::string& expected) {
        std::scoped_lock lock(mutex);
        const auto directory = projectPath(root, journalDirectory);
        // Before the commit point: the journal directory must itself survive a power cut.
        createDirectoriesDurably(root, directory);
        // From here to the end, no other process recovers, checks the precondition or publishes.
        ProjectLock processLock{directory / lockName};
        recoverHoldingLock();
        auto group = checkedGroup(ids, paths);
        if (fingerprint(root, paths) != expected)
            throw std::runtime_error("The project changed since it was read; run the command again");
        const auto number = prepareJournal(directory, group);
        try {
            writeJournal(directory, number, group);
        } catch (const InjectedFault&) {
            throw;
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(directory / (number + ".tmp"), ignored);
            std::filesystem::remove(directory / (number + ".journal"), ignored);
            returnStaged(directory / (number + ".d"), group);
            throw;
        }
        std::vector<JournalEntry> entries;
        for (std::size_t i = 0; i < group.size(); ++i)
            entries.push_back({group[i]->destination, std::to_string(i)});
        for (const auto& id : ids) {
            std::error_code ignored;
            std::filesystem::remove_all(transactions.at(id).staging, ignored);
            transactions.erase(id);
        }
        try {
            rollForward(root, directory, number, entries, hook);
        } catch (const InjectedFault&) {
            throw;
        } catch (const std::exception& exception) {
            IncompletePublication incomplete{exception.what()};
            pendingRecovery = incomplete.what();
            throw incomplete;
        }
    }

    /// Bridge forms of state() and commitGroup(): one path or identifier per line.
    std::string stateOfLines(std::string paths) {
        return state(lines(paths));
    }
    void commitGroupLines(std::string ids, std::string paths, std::string expected) {
        commitGroup(lines(ids), lines(paths), expected);
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
    // The listed form of one entry, or nothing when it leaves the project or its type cannot be read.
    std::optional<std::string> listedName(const std::string& directory, const std::string& name) const {
        std::error_code error;
        try {
            const auto resolved = projectPath(root, directory.empty() ? name : directory + "/" + name);
            const bool isDirectory = std::filesystem::is_directory(resolved, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return std::nullopt;
            return isDirectory ? name + "/" : name;
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        } catch (const std::filesystem::filesystem_error&) {
            return std::nullopt;
        }
    }

    static std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> result;
        std::istringstream       stream{text};
        for (std::string line; std::getline(stream, line);)
            if (!line.empty())
                result.push_back(std::move(line));
        return result;
    }

    struct Transaction;
    // The transactions of a group: single files, distinct, each destination covered by the precondition.
    std::vector<Transaction*> checkedGroup(const std::vector<std::string>& ids, const std::vector<std::string>& paths) {
        if (ids.empty() || ids.size() > maxGroupFiles || paths.empty() || paths.size() > maxGroupFiles * 2)
            throw std::invalid_argument("Invalid group publication");
        std::vector<Transaction*> group;
        for (const auto& id : ids) {
            auto& tx = transaction(id);
            if (std::ranges::find(group, &tx) != group.end() || tx.directory)
                throw std::invalid_argument("A group publishes distinct single files");
            if (std::ranges::find(paths, tx.destination) == paths.end())
                throw std::invalid_argument("A group precondition must cover every destination");
            if (projectPath(root, tx.destination) != tx.target)
                throw std::runtime_error("Project path changed");
            if (!tx.replace && std::filesystem::exists(std::filesystem::symlink_status(tx.target)))
                throw std::runtime_error("Starter destination already exists");
            stagedSource(tx);
            group.push_back(&tx);
        }
        return group;
    }

    // Moves the staged files into a directory owned by the journal and makes them durable.
    std::string prepareJournal(const std::filesystem::path& directory, const std::vector<Transaction*>& group) {
        for (;;) {
            const auto number = std::to_string(++stagingCounter);
            if (std::filesystem::exists(directory / (number + ".journal")) || std::filesystem::exists(directory / (number + ".tmp")))
                continue;
            if (!std::filesystem::create_directory(directory / (number + ".d")))
                continue;
            try {
                for (std::size_t i = 0; i < group.size(); ++i) {
                    const auto owned = directory / (number + ".d") / std::to_string(i);
                    std::filesystem::rename(stagedSource(*group[i]), owned);
                    syncFile(owned);
                }
                syncDirectory(directory / (number + ".d"));
            } catch (const InjectedFault&) {
                throw;
            } catch (...) {
                returnStaged(directory / (number + ".d"), group);
                throw;
            }
            return number;
        }
    }

    void writeJournal(const std::filesystem::path& directory, const std::string& number, const std::vector<Transaction*>& group) {
        std::vector<JournalEntry> entries;
        for (std::size_t i = 0; i < group.size(); ++i)
            entries.push_back({group[i]->destination, std::to_string(i)});
        step("before-journal");
        const auto temporary = directory / (number + ".tmp");
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            const auto    text = serializeJournal(entries);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            if (!file)
                throw std::runtime_error("Cannot write the publication journal");
        }
        syncFile(temporary);
        std::filesystem::rename(temporary, directory / (number + ".journal"));
        syncDirectory(directory);
        step("after-journal");
    }

    // Before the commit point: puts the files moved into a journal directory back in their transactions.
    static void returnStaged(const std::filesystem::path& owned, const std::vector<Transaction*>& group) {
        std::error_code ignored;
        for (std::size_t i = 0; i < group.size(); ++i) {
            const auto moved = owned / std::to_string(i);
            if (std::filesystem::is_regular_file(moved, ignored))
                std::filesystem::rename(moved, group[i]->staging / "output", ignored);
        }
        std::filesystem::remove_all(owned, ignored);
    }

    // Completes the committed publications, recording why one cannot be completed.
    template<typename Recovery> void recordRecovery(Recovery recovery) {
        try {
            recovery(root, hook);
            pendingRecovery.reset();
        } catch (const IncompletePublication& incomplete) {
            pendingRecovery = incomplete.what();
            throw;
        }
    }
    void recover() {
        recordRecovery(recoverJournals);
    }
    void recoverHoldingLock() {
        recordRecovery(recoverLocked);
    }

    void step(std::string_view name) const {
        if (hook)
            hook(name);
    }

    struct Transaction {
        std::filesystem::path                staging;
        std::filesystem::path                target;
        std::filesystem::path                resolved;
        std::string                          destination;
        bool                                 directory;
        bool                                 replace;
        std::uint64_t                        size;
        std::map<std::string, std::uint64_t> files;
        std::set<std::string>                folded;
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

    // Two paths differing only by case are one file on Windows and macOS: refuse them instead of concatenating.
    static void requireDistinctCase(Transaction& tx, const std::string& key) {
        std::string lowered = key;
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tx.files.contains(key))
            return;
        if (!tx.folded.insert(lowered).second)
            throw std::invalid_argument("Developer paths differing only by case: " + key);
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
    StepHook                                 hook;
    std::optional<std::string>               pendingRecovery;
    mutable std::mutex                       mutex;
    std::map<std::string, Transaction>       transactions;
};

}  // namespace webfront::developer
