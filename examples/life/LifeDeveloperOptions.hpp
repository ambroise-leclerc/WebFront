#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace webfront::demo {

/// Command line of WebFrontLifeDeveloper: `[<project> [<environment-tarball-url> <sha512-integrity>]]`.
struct LifeDeveloperOptions {
    static constexpr const char* defaultProject = "life-project";

    std::filesystem::path       project = defaultProject;
    std::optional<std::string>  environment;
    std::optional<std::string>  integrity;
};

/// Returns nullopt when the arguments do not follow the usage; no argument selects `life-project` in the
/// current directory and the pinned developer environment.
[[nodiscard]] inline std::optional<LifeDeveloperOptions> parseLifeDeveloperOptions(int argc, const char* const* argv) {
    if (argc != 1 && argc != 2 && argc != 4)
        return std::nullopt;
    LifeDeveloperOptions options;
    if (argc >= 2) {
        if (*argv[1] == '\0')
            return std::nullopt;
        options.project = argv[1];
    }
    if (argc == 4) {
        options.environment = argv[2];
        options.integrity   = argv[3];
    }
    return options;
}

/// Creates the project directory when it is missing and leaves an existing one untouched.
/// Throws std::filesystem::filesystem_error when the path cannot be a directory.
inline void prepareLifeProject(const std::filesystem::path& project) {
    std::filesystem::create_directories(project);
}

}  // namespace webfront::demo
