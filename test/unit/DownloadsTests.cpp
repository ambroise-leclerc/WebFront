#include <catch2/catch_test_macros.hpp>
#include <webfront/frontend/Downloads.hpp>

#include <filesystem>
#include <fstream>

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
