/**
 * @file PathUtils.hpp
 * @brief Header-only utilities for path resolution and file discovery
 * 
 * This header-only library provides utilities for finding files in various
 * search paths, particularly useful for locating resource files in development
 * and testing contexts.
 */

#pragma once

#include <tooling/Logger.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>

namespace webfront::tooling {

/**
 * @brief Find a file in a list of search paths relative to the WebFront project root
 * 
 * Search strategy:
 * 1. Current working directory
 * 2. System temp directory
 * 3. WebFront project subdirectories (configurable)
 * 
 * @param filename Name of the file to find
 * @param searchPaths List of subdirectories to search within WebFront project
 * @return std::filesystem::path Path to the directory containing the file
 * @throws std::runtime_error If file cannot be found in any location
 */
inline std::filesystem::path findFileInPaths(
    const std::string& filename,
    const std::vector<std::string>& searchPaths = {"src", "webtest"}
) {
    using namespace std::filesystem;

    // 1. Check current directory
    log::info("Looking for {} in {}", filename, current_path().string());
    if (exists(current_path() / filename)) {
        return current_path();
    }

    // 2. Check temp directory
    log::info("  not found : Looking now in temp directory {}", temp_directory_path().string());
    if (exists(temp_directory_path() / filename)) {
        return temp_directory_path();
    }

    // 3. Check WebFront project subdirectories
    path webfront;
    const auto currentPath{current_path()};
    for (const auto& element : currentPath) {
        webfront = webfront / element;
        static constexpr std::string_view webFrontRootPath = "WebFront";
        if (element == webFrontRootPath) {
            // Try each configured search path
            for (const auto& searchPath : searchPaths) {
            path candidate = webfront / searchPath;
            log::info("  not found : Looking now in source directory {}", candidate.string());
            if (exists(candidate / filename)) {
                log::info("Found {} in {}", filename, candidate.string());
                return candidate;
            }
            }
        }
    }

    throw std::runtime_error("Cannot find " + filename + " file");
}

/**
 * @brief Find document root for main application files
 * 
 * Searches in current directory, temp directory, and WebFront/src
 * 
 * @param filename Name of the file to find
 * @return std::filesystem::path Path to the directory containing the file
 */
inline std::filesystem::path findDocRoot(const std::string& filename) {
    return findFileInPaths(filename, {"src"});
}

/**
 * @brief Find test root for test-related files
 * 
 * Searches in current directory, temp directory, and WebFront/webtest
 * 
 * @param filename Name of the file to find
 * @return std::filesystem::path Path to the directory containing the file
 */
inline std::filesystem::path findTestRoot(const std::string& filename) {
    return findFileInPaths(filename, {"webtest"});
}

/**
 * @brief Find file in both development and test directories
 * 
 * Searches in current directory, temp directory, WebFront/src, and WebFront/webtest
 * 
 * @param filename Name of the file to find
 * @return std::filesystem::path Path to the directory containing the file
 */
inline std::filesystem::path findAnyRoot(const std::string& filename) {
    return findFileInPaths(filename, {"src", "webtest"});
}

} // namespace webfront::tooling