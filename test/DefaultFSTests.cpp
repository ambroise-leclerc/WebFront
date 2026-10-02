#include <system/DefaultFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace std;
using namespace webfront;

namespace {

// Minimal temp-directory fixture, following the convention used in test/NativeFSTests.cpp.
class TemporaryRoot {
public:
    TemporaryRoot() {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> distrib(1000, 9999);
        dir = filesystem::temp_directory_path() / ("defaultfs_test_" + to_string(distrib(gen)));
        filesystem::create_directories(dir);
    }
    ~TemporaryRoot() {
        error_code ec;
        filesystem::remove_all(dir, ec);
    }

    void write(const string& relativePath, const string& content) const {
        ofstream file(dir / relativePath, ios::binary);
        file << content;
    }

    filesystem::path path() const { return dir; }

private:
    filesystem::path dir;
};

string readAll(fs::File& file) {
    string content;
    array<char, 512> buffer;
    while (auto bytesRead = file.read(buffer)) content.append(buffer.data(), bytesRead);
    return content;
}

} // namespace

SCENARIO("DefaultFS serves only embedded assets when no document root is configured") {
    GIVEN("a DefaultFS constructed with an empty document root") {
        fs::DefaultFS defaultFS{filesystem::path{}};

        THEN("the embedded WebFront.js, index.html, and favicon.ico are served") {
            REQUIRE(defaultFS.open("WebFront.js").has_value());
            REQUIRE(defaultFS.open("index.html").has_value());
            REQUIRE(defaultFS.open("favicon.ico").has_value());
        }

        THEN("an arbitrary file is not found, and no local filesystem access is attempted") {
            REQUIRE_FALSE(defaultFS.open("does-not-exist.txt").has_value());
        }
    }
}

SCENARIO("DefaultFS layers a configured document root over the embedded fallback") {
    GIVEN("a document root containing a custom index.html") {
        TemporaryRoot root;
        root.write("index.html", "<!DOCTYPE html><title>Custom</title>");
        fs::DefaultFS defaultFS{root.path()};

        WHEN("index.html is requested") {
            auto file = defaultFS.open("index.html");
            THEN("the custom file overrides the embedded fallback page") {
                REQUIRE(file.has_value());
                REQUIRE(readAll(*file) == "<!DOCTYPE html><title>Custom</title>");
            }
        }
    }

    GIVEN("a document root that also contains a file named WebFront.js") {
        TemporaryRoot root;
        root.write("WebFront.js", "// not the real bridge script");
        fs::DefaultFS defaultFS{root.path()};

        WHEN("WebFront.js is requested") {
            auto file = defaultFS.open("WebFront.js");
            THEN("the embedded, version-matched script is served instead of the local file") {
                REQUIRE(file.has_value());
                REQUIRE(readAll(*file) != "// not the real bridge script");
            }
        }
    }

    GIVEN("a document root containing a file not otherwise served") {
        TemporaryRoot root;
        root.write("app.mjs", "export const ready = true;");
        fs::DefaultFS defaultFS{root.path()};

        WHEN("app.mjs is requested") {
            auto file = defaultFS.open("app.mjs");
            THEN("the local development file is served") {
                REQUIRE(file.has_value());
                REQUIRE(readAll(*file) == "export const ready = true;");
            }
        }
    }
}
