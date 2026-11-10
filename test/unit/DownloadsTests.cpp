#include <catch2/catch_test_macros.hpp>
#include <webfront/frontend/Downloads.hpp>

#include <filesystem>
#include <cstddef>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

using webfront::frontend::detail::DownloadReservations;
using webfront::frontend::detail::uniqueDownloadPath;

SCENARIO("Download staging preserves a destination created while a download runs") {
    std::random_device random;
    const auto         folder = std::filesystem::temp_directory_path() / ("webfront-download-staging-unit-" + std::to_string(random()));
    REQUIRE(std::filesystem::create_directory(folder));
    const auto target = folder / "target.txt";
    GIVEN("another process creates the destination during the download") {
        WHEN("the completed download is installed without permission to replace") {
            std::filesystem::path stagingPath;
            bool                  installed = false;
            {
                webfront::frontend::detail::DownloadStaging staging(folder);
                stagingPath = staging.path();
                std::ofstream(stagingPath) << "download";
                std::ofstream(target) << "other file";
                installed = staging.finish(target, false);
            }
            THEN("the other file survives and the private staging directory is removed") {
                REQUIRE_FALSE(installed);
                std::ifstream stream(target);
                REQUIRE(std::string(std::istreambuf_iterator<char>(stream), {}) == "other file");
                REQUIRE_FALSE(std::filesystem::exists(stagingPath.parent_path()));
            }
        }
    }
    GIVEN("the destination is free") {
        WHEN("the completed download is installed") {
            std::filesystem::path stagingPath;
            bool                  installed = false;
            {
                webfront::frontend::detail::DownloadStaging staging(folder);
                stagingPath = staging.path();
                std::ofstream(stagingPath) << "completed";
                installed = staging.finish(target, false);
            }
            THEN("the destination holds the completed bytes and staging is removed") {
                REQUIRE(installed);
                std::ifstream completed(target);
                REQUIRE(std::string(std::istreambuf_iterator<char>(completed), {}) == "completed");
                REQUIRE_FALSE(std::filesystem::exists(stagingPath.parent_path()));
            }
        }
    }
    std::filesystem::remove(target);
    std::filesystem::remove(folder);
}

SCENARIO("A download name suggested by a page stays inside the download folder") {
    const auto folder = std::filesystem::temp_directory_path() / "webfront-downloads-unit";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    GIVEN("names that try to leave the folder or name nothing") {
        THEN("only the last component is kept, and an empty name becomes \"download\"") {
            REQUIRE(uniqueDownloadPath(folder, "report.txt") == folder / "report.txt");
            REQUIRE(uniqueDownloadPath(folder, "../../etc/passwd") == folder / "passwd");
            REQUIRE(uniqueDownloadPath(folder, "a/b/c.bin") == folder / "c.bin");
            for (const auto* name : {"", ".", "..", "dir/.."}) {
                INFO(name);
                REQUIRE(uniqueDownloadPath(folder, name) == folder / "download");
            }
        }
    }
    GIVEN("files that already exist") {
        std::ofstream(folder / "report.txt") << "1";
        THEN("the name is numbered rather than overwritten") {
            REQUIRE(uniqueDownloadPath(folder, "report.txt") == folder / "report (1).txt");
            std::ofstream(folder / "report (1).txt") << "2";
            REQUIRE(uniqueDownloadPath(folder, "report.txt") == folder / "report (2).txt");
        }
    }
    GIVEN("every candidate is reserved by another download") {
        THEN("exhaustion is reported instead of returning an occupied path") {
            std::size_t probes = 0;
            REQUIRE_THROWS_AS(uniqueDownloadPath(folder,
                                                 "report.txt",
                                                 [&](const auto&) {
                                                     ++probes;
                                                     return true;
                                                 }),
                              std::filesystem::filesystem_error);
            REQUIRE(probes == 10'000);
        }
    }
    GIVEN("only the last candidate is available") {
        THEN("that candidate can still be used") {
            REQUIRE(uniqueDownloadPath(folder,
                                       "report.txt",
                                       [&](const auto& path) {
                                           return path.filename() != "report (9999).txt";
                                       })
                    == folder / "report (9999).txt");
        }
    }
    std::filesystem::remove_all(folder);
}

SCENARIO("Downloads that ask for the same name before their files exist get different destinations") {
    const auto folder = std::filesystem::temp_directory_path() / "webfront-downloads-reserved";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    DownloadReservations reservations;
    GIVEN("two downloads suggesting report.txt, neither file created yet") {
        const auto first  = reservations.reserve(folder, "report.txt");
        const auto second = reservations.reserve(folder, "report.txt");
        THEN("the second one is numbered") {
            REQUIRE(first == folder / "report.txt");
            REQUIRE(second == folder / "report (1).txt");
            REQUIRE(reservations.reserve(folder, "report.txt") == folder / "report (2).txt");
        }
        AND_WHEN("the first download ends without creating a file") {
            reservations.release(first);
            THEN("its name is available again") {
                REQUIRE(reservations.reserve(folder, "report.txt") == first);
            }
        }
        AND_WHEN("a file appears under a released name") {
            reservations.release(first);
            std::ofstream(first) << "done";
            THEN("it is the existing file that keeps the name taken") {
                REQUIRE(reservations.reserve(folder, "report.txt") == folder / "report (2).txt");
            }
        }
    }
    GIVEN("many threads asking for the same name at once") {
        constexpr std::size_t                threads = 16;
        std::vector<std::filesystem::path> granted(threads);
        std::vector<std::thread>           workers;
        for (std::size_t index = 0; index < threads; ++index)
            workers.emplace_back([&, index] {
                granted[index] = reservations.reserve(folder, "same.bin");
            });
        for (auto& worker : workers)
            worker.join();
        THEN("every destination is distinct") {
            REQUIRE(std::set<std::filesystem::path>(granted.begin(), granted.end()).size() == threads);
        }
    }
    GIVEN("released reservations") {
        const auto path = reservations.reserve(folder, "once.txt");
        reservations.release(path);
        THEN("nothing stays reserved") {
            DownloadReservations other;
            const auto           again = other.reserve(folder, "x");
            other.release(again);
            REQUIRE(other.empty());
        }
    }
    std::filesystem::remove_all(folder);
}
