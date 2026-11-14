#include "Bdd.hpp"
#include <webfront/frontend/Downloads.hpp>

#include <filesystem>
#include <cstddef>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

using namespace webfront::bdd;

using webfront::frontend::detail::DownloadReservations;
using webfront::frontend::detail::uniqueDownloadPath;

Scenario downloadStaging{"Download staging preserves a destination created while a download runs", [] {
    std::random_device random;
    const auto         folder = std::filesystem::temp_directory_path() / ("webfront-download-staging-unit-" + std::to_string(random()));
    require(std::filesystem::create_directory(folder));
    const auto target = folder / "target.txt";
    given("another process creates the destination during the download", [&] {
        when("the completed download is installed without permission to replace", [&] {
            std::filesystem::path stagingPath;
            bool                  installed = false;
            {
                webfront::frontend::detail::DownloadStaging staging(folder);
                stagingPath = staging.path();
                std::ofstream(stagingPath) << "download";
                std::ofstream(target) << "other file";
                installed = staging.finish(target, false);
            }
            then("the other file survives and the private staging directory is removed", [&] {
                require(!installed);
                std::ifstream stream(target);
                require(std::string(std::istreambuf_iterator<char>(stream), {}) == "other file");
                require(!std::filesystem::exists(stagingPath.parent_path()));
            });
        });
    });
    given("the destination is free", [&] {
        when("the completed download is installed", [&] {
            std::filesystem::path stagingPath;
            bool                  installed = false;
            {
                webfront::frontend::detail::DownloadStaging staging(folder);
                stagingPath = staging.path();
                std::ofstream(stagingPath) << "completed";
                installed = staging.finish(target, false);
            }
            then("the destination holds the completed bytes and staging is removed", [&] {
                require(installed);
                std::ifstream completed(target);
                require(std::string(std::istreambuf_iterator<char>(completed), {}) == "completed");
                require(!std::filesystem::exists(stagingPath.parent_path()));
            });
        });
    });
    std::filesystem::remove(target);
    std::filesystem::remove(folder);
}};

Scenario downloadName{"A download name suggested by a page stays inside the download folder", [] {
    const auto folder = std::filesystem::temp_directory_path() / "webfront-downloads-unit";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    given("names that try to leave the folder or name nothing", [&] {
        then("only the last component is kept, and an empty name becomes \"download\"", [&] {
            require(uniqueDownloadPath(folder, "report.txt") == folder / "report.txt");
            require(uniqueDownloadPath(folder, "../../etc/passwd") == folder / "passwd");
            require(uniqueDownloadPath(folder, "a/b/c.bin") == folder / "c.bin");
            for (const auto* name : {"", ".", "..", "dir/.."}) {
                auto scope = info(name);
                require(uniqueDownloadPath(folder, name) == folder / "download");
            }
        });
    });
    given("files that already exist", [&] {
        std::ofstream(folder / "report.txt") << "1";
        then("the name is numbered rather than overwritten", [&] {
            require(uniqueDownloadPath(folder, "report.txt") == folder / "report (1).txt");
            std::ofstream(folder / "report (1).txt") << "2";
            require(uniqueDownloadPath(folder, "report.txt") == folder / "report (2).txt");
        });
    });
    given("every candidate is reserved by another download", [&] {
        then("exhaustion is reported instead of returning an occupied path", [&] {
            std::size_t probes = 0;
            requireThrows<std::filesystem::filesystem_error>([&] { uniqueDownloadPath(folder,
                                                 "report.txt",
                                                 [&](const auto&) {
                                                     ++probes;
                                                     return true;
                                                 }); });
            require(probes == 10'000);
        });
    });
    given("only the last candidate is available", [&] {
        then("that candidate can still be used", [&] {
            require(uniqueDownloadPath(folder,
                                       "report.txt",
                                       [&](const auto& path) {
                                           return path.filename() != "report (9999).txt";
                                       })
                    == folder / "report (9999).txt");
        });
    });
    std::filesystem::remove_all(folder);
}};

Scenario downloadsAsk{"Downloads that ask for the same name before their files exist get different destinations", [] {
    const auto folder = std::filesystem::temp_directory_path() / "webfront-downloads-reserved";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    DownloadReservations reservations;
    given("two downloads suggesting report.txt, neither file created yet", [&] {
        const auto first  = reservations.reserve(folder, "report.txt");
        const auto second = reservations.reserve(folder, "report.txt");
        then("the second one is numbered", [&] {
            require(first == folder / "report.txt");
            require(second == folder / "report (1).txt");
            require(reservations.reserve(folder, "report.txt") == folder / "report (2).txt");
        });
        andWhen("the first download ends without creating a file", [&] {
            reservations.release(first);
            then("its name is available again", [&] {
                require(reservations.reserve(folder, "report.txt") == first);
            });
        });
        andWhen("a file appears under a released name", [&] {
            reservations.release(first);
            std::ofstream(first) << "done";
            then("it is the existing file that keeps the name taken", [&] {
                require(reservations.reserve(folder, "report.txt") == folder / "report (2).txt");
            });
        });
    });
    given("many threads asking for the same name at once", [&] {
        constexpr std::size_t                threads = 16;
        std::vector<std::filesystem::path> granted(threads);
        std::vector<std::thread>           workers;
        for (std::size_t index = 0; index < threads; ++index)
            workers.emplace_back([&, index] {
                granted[index] = reservations.reserve(folder, "same.bin");
            });
        for (auto& worker : workers)
            worker.join();
        then("every destination is distinct", [&] {
            require(std::set<std::filesystem::path>(granted.begin(), granted.end()).size() == threads);
        });
    });
    given("released reservations", [&] {
        const auto path = reservations.reserve(folder, "once.txt");
        reservations.release(path);
        then("nothing stays reserved", [&] {
            DownloadReservations other;
            const auto           again = other.reserve(folder, "x");
            other.release(again);
            require(other.empty());
        });
    });
    std::filesystem::remove_all(folder);
}};
