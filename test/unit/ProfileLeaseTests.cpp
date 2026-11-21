#include "Bdd.hpp"
#include <webfront/frontend/ProfileLease.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace webfront::bdd;
using namespace webfront;

namespace {

std::filesystem::path scratch(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("webfront-lease-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::remove_all(path.string() + ".instances");
    return path;
}

}  // namespace

#ifndef _WIN32
const Scenario profileLease{"A browser profile is never used by two live instances", [] {
    given("a profile directory", [&] {
        const auto profile = scratch("profile");
        then("the first lease keeps it and the next ones get a directory of their own", [&] {
            const auto first = frontend::leaseProfileDirectory(profile);
            require(first == profile);
            std::set<std::filesystem::path> others;
            for (int i = 0; i < 20; ++i) {
                const auto other = frontend::leaseProfileDirectory(profile);
                require(other != profile && std::filesystem::is_directory(other));
                require(other.parent_path() == std::filesystem::path(profile.string() + ".instances"));
                others.insert(other);
            }
            require(others.size() == 20);
        });
        std::filesystem::remove_all(profile);
        std::filesystem::remove_all(profile.string() + ".instances");
    });
    given("the directory of an instance that died without cleaning up", [&] {
        const auto profile = scratch("stale");
        const auto dead    = std::filesystem::path(profile.string() + ".instances") / "1-0";
        std::filesystem::create_directories(dead);
        std::ofstream{dead / "data"} << "left behind";
        then("the next lease removes it", [&] {
            (void)frontend::leaseProfileDirectory(profile);  // takes the profile itself
            (void)frontend::leaseProfileDirectory(profile);  // scans the instances
            require(!std::filesystem::exists(dead));
        });
        std::filesystem::remove_all(profile);
        std::filesystem::remove_all(profile.string() + ".instances");
    });
}};
#endif
