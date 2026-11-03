#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __APPLE__
    #include <unistd.h>
#endif

namespace webfront::frontend::detail {

/** A private staging directory beside a download target. */
class DownloadStaging {
public:
    explicit DownloadStaging(const std::filesystem::path& parent) {
        sweepStale(parent);
#ifdef __APPLE__
        auto pattern = (parent / ".webfront-download-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (!::mkdtemp(buffer.data()))
            throw std::filesystem::filesystem_error("Cannot create download staging", parent, std::error_code(errno, std::generic_category()));
        directory = buffer.data();
#else
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = parent / (".webfront-download-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory = std::move(candidate);
                std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
                if (error) {
                    std::filesystem::remove(directory);
                    throw std::filesystem::filesystem_error("Cannot secure download staging", directory, error);
                }
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("Cannot create download staging", candidate, error);
        }
        throw std::filesystem::filesystem_error("Cannot allocate download staging", parent, std::make_error_code(std::errc::file_exists));
#endif
    }
    DownloadStaging(const DownloadStaging&) = delete;
    DownloadStaging& operator=(const DownloadStaging&) = delete;
    ~DownloadStaging() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    [[nodiscard]] std::filesystem::path path() const { return directory / "download"; }
    /**
     * @brief Installs the staged file. A new target never replaces a file that appeared meanwhile.
     *
     * A hard link installs atomically; on a filesystem without links (exFAT, FAT, some network shares) the file is copied,
     * which still refuses an existing target. A copy that failed midway is removed.
     */
    bool finish(const std::filesystem::path& target, bool replace) const {
        std::error_code error;
        if (replace) {
            std::filesystem::rename(path(), target, error);
            return !error;
        }
        std::filesystem::create_hard_link(path(), target, error);
        if (!error)
            return true;
        error.clear();
        std::filesystem::copy_file(path(), target, std::filesystem::copy_options::none, error);
        if (error && error != std::errc::file_exists) {
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
        }
        return !error;
    }

    /// Removes the staging directories that a crash left in `parent` more than a week ago.
    static void sweepStale(const std::filesystem::path& parent) noexcept {
        try {
            constexpr std::string_view prefix = ".webfront-download-";
            const auto                 limit  = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
            std::error_code            error;
            for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
                const auto name = it->path().filename().string();
                if (!name.starts_with(prefix) || !it->is_directory(error))
                    continue;
                if (const auto written = std::filesystem::last_write_time(it->path(), error); !error && written < limit) {
                    std::error_code ignored;
                    std::filesystem::remove_all(it->path(), ignored);
                }
                error.clear();
            }
        } catch (...) {
        }
    }

private:
    std::filesystem::path directory;
};

/**
 * @brief A path in `folder` that does not exist yet, for a download whose name the page suggested.
 *
 * Only the final component of the suggested name is kept (a page can neither choose a folder nor climb out of it), an
 * empty name becomes "download", and an existing file is kept: "name", then "name (1).ext", "name (2).ext", and so on.
 * `taken` additionally rejects the names that another download has reserved but not created yet.
 * Throws `std::filesystem::filesystem_error` if all 10,000 candidates are occupied.
 */
inline std::filesystem::path
uniqueDownloadPath(const std::filesystem::path& folder, std::string_view suggested, const std::function<bool(const std::filesystem::path&)>& taken = {}) {
    auto name = std::filesystem::path(std::string(suggested)).filename();
    // Leading dots would let a page create hidden files (".bash_login") when the folder is the home folder.
    if (const auto text = name.string(); text.starts_with('.'))
        name = text.substr(text.find_first_not_of('.') == std::string::npos ? text.size() : text.find_first_not_of('.'));
    if (name.empty() || name == "." || name == "..")
        name = "download";
    auto            result = folder / name;
    std::error_code error;
    const auto      unavailable = [&](const std::filesystem::path& path) {
        error.clear();
        const bool exists = std::filesystem::exists(path, error);
        return exists || error || (taken && taken(path));  // An unreadable path is not known to be free.
    };
    for (int index = 1; unavailable(result); ++index) {
        if (index == 10'000)
            throw std::filesystem::filesystem_error("No free download destination", folder / name, std::make_error_code(std::errc::file_exists));
        result = folder / (name.stem().string() + " (" + std::to_string(index) + ")" + name.extension().string());
    }
    return result;
}

/**
 * @brief The destinations handed to downloads that have not created their file yet.
 *
 * A name is free only if no file has it and no running download was given it: two downloads that ask for the same name
 * before either file exists get "name" and "name (1)", never the same path. Choosing and reserving happen under one lock;
 * release a path when its download ends (finished, failed or cancelled) so that the name can be used again.
 */
class DownloadReservations {
public:
    [[nodiscard]] std::filesystem::path reserve(const std::filesystem::path& folder, std::string_view suggested) {
        std::scoped_lock lock{mutex};
        auto             result = uniqueDownloadPath(folder, suggested, [&](const std::filesystem::path& path) {
            return reserved.contains(path);
        });
        reserved.insert(result);
        return result;
    }

    /// Reserves exactly `path` (a destination the user chose); false if another running download already holds it.
    [[nodiscard]] bool reserveExact(const std::filesystem::path& path) {
        std::scoped_lock lock{mutex};
        return reserved.insert(path).second;
    }

    void release(const std::filesystem::path& path) {
        std::scoped_lock lock{mutex};
        reserved.erase(path);
    }

    [[nodiscard]] bool empty() const {
        std::scoped_lock lock{mutex};
        return reserved.empty();
    }

private:
    mutable std::mutex         mutex;
    std::set<std::filesystem::path> reserved;
};

}  // namespace webfront::frontend::detail
