#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace webfront::test {

/// A new, empty directory under the system temporary directory, removed with its content on
/// destruction.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned> counter{0};
        const auto                   base = std::filesystem::temp_directory_path();
        std::random_device           entropy;
        do
            directory = base / ("webfront_test_" + std::to_string(entropy()) + "_" + std::to_string(counter++));
        while (!std::filesystem::create_directory(directory));
    }
    TemporaryDirectory(const TemporaryDirectory&)            = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return directory;
    }

private:
    std::filesystem::path directory;
};

}  // namespace webfront::test
