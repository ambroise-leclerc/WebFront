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
