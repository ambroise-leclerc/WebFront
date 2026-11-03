#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace webfront::frontend::detail {

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
    if (name.empty() || name == "." || name == "..")
        name = "download";
    auto            result = folder / name;
    std::error_code error;
    const auto      unavailable = [&](const std::filesystem::path& path) {
        return std::filesystem::exists(path, error) || (taken && taken(path));
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
