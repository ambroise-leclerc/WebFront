#include <webfront/testing/Bdd.hpp>
#include <webfront/frontend/ProfileLease.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace webfront::bdd;
using namespace webfront;

#ifndef _WIN32
namespace {

std::filesystem::path scratch(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("webfront-lease-" + std::to_string(::getpid()) + "-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::remove_all(path.string() + ".instances");
    return path;
}

}  // namespace

const Scenario profileLease{"A browser profile is never used by two live instances", [] {
    given("a profile directory", [&] {
        const auto profile = scratch("profile");
        then("the process keeps the profile, and finds its data again when it asks again", [&] {
            const auto first = frontend::leaseProfileDirectory(profile);
            require(first == profile);
            std::ofstream{first / "data"} << "saved by the first window";
            const auto reopened = frontend::leaseProfileDirectory(profile);
            require(reopened == first && std::filesystem::exists(reopened / "data"));
            require(!std::filesystem::exists(profile.string() + ".instances"));
        });
        std::filesystem::remove_all(profile);
    });
    given("the directory of an instance that died without cleaning up", [&] {
        const auto profile = scratch("stale");
        const auto dead    = std::filesystem::path(profile.string() + ".instances") / "1-0";
        std::filesystem::create_directories(dead);
        std::ofstream{dead / "data"} << "left behind";
        then("the next lease removes it", [&] {
            (void)frontend::leaseProfileDirectory(profile);  // takes the profile itself and clears the abandoned instances
            require(!std::filesystem::exists(dead));
        });
        std::filesystem::remove_all(profile);
        std::filesystem::remove_all(profile.string() + ".instances");
    });

    given("a lock file that cannot be taken for a reason other than contention", [&] {
        const auto profile = scratch("broken");
        std::filesystem::create_directories(profile / ".webfront-lock");  // a directory where the lock file should be
        then("the error is reported, not retried with new directories", [&] {
            requireThrows<std::system_error>([&] { (void)frontend::leaseProfileDirectory(profile); }, contains(".webfront-lock"));
            require(!std::filesystem::exists(profile.string() + ".instances"));
        });
        std::filesystem::remove_all(profile);
    });
}};

const Scenario concurrentProfileLeases{"Processes that start at the same time lease different profiles", [] {
    given("a profile with directories left by dead instances, and several processes starting together", [&] {
        const auto profile = scratch("concurrent");
        const auto instances = std::filesystem::path(profile.string() + ".instances");
        for (int i = 0; i < 5; ++i) {
            std::filesystem::create_directories(instances / ("dead-" + std::to_string(i)));
            std::ofstream{instances / ("dead-" + std::to_string(i)) / ".webfront-lock"};
        }
        constexpr int          count = 24;
        std::vector<pid_t>     children;
        std::vector<int>       reports, releases;
        int                    start[2];
        require(::pipe(start) == 0);
        for (int i = 0; i < count; ++i) {
            int report[2], release[2];
            require(::pipe(report) == 0 && ::pipe(release) == 0);
            const pid_t child = ::fork();
            require(child >= 0);
            if (child == 0) {
                // Only async-signal-unsafe work the test needs; the process leaves with _exit, so no static destructor runs.
                ::close(start[1]);
                ::close(report[0]);
                ::close(release[1]);  // its own pipe's other ends, or the end-of-file of `release` would never come
                for (const int inherited : reports)  // the earlier children's pipes: their end-of-file must not wait for this process
                    ::close(inherited);
                for (const int inherited : releases)
                    ::close(inherited);
                char go;
                (void)::read(start[0], &go, 1);  // all the children begin together, when the parent closes the write end
                std::string result;
                try {
                    result = frontend::leaseProfileDirectory(profile).string();
                } catch (const std::exception& error) {
                    result = std::string("error: ") + error.what();
                }
                result += '\n';
                (void)::write(report[1], result.data(), result.size());
                char hold;
                (void)::read(release[0], &hold, 1);  // alive until the parent has looked at every directory
                ::_exit(0);
            }
            ::close(report[1]);
            ::close(release[0]);
            children.push_back(child);
            reports.push_back(report[0]);
            releases.push_back(release[1]);
        }
        ::close(start[0]);
        ::close(start[1]);  // releases the children
        std::set<std::string> paths;
        for (const int fd : reports) {
            std::string line;
            char        c;
            while (::read(fd, &c, 1) == 1 && c != '\n')
                line += c;
            paths.insert(line);
        }
        then("each process has a directory of its own, which exists with its lock file", [&] {
            require(paths.size() == count);
            for (const auto& path : paths) {
                require(path.rfind("error", 0) != 0);
                require(std::filesystem::is_directory(path));
                require(std::filesystem::exists(std::filesystem::path(path) / ".webfront-lock"));
            }
            require(paths.contains(profile.string()));
            for (int i = 0; i < 5; ++i)
                require(!std::filesystem::exists(instances / ("dead-" + std::to_string(i))));
        });
        for (const int fd : releases)
            ::close(fd);  // the children end
        for (const pid_t child : children) {
            int status = 0;
            ::waitpid(child, &status, 0);
        }
        std::filesystem::remove_all(profile);
        std::filesystem::remove_all(instances);
    });
}};
#endif
