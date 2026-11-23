#include "TemporaryDirectory.hpp"
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

namespace {

// Each fixture atomically claims its own temporary directory across CTest processes.
class TemporaryRoot {
public:
    void write(const string& relativePath, const string& content) const {
        ofstream file(root.path() / relativePath, ios::binary);
        file << content;
    }

    filesystem::path path() const {
        return root.path();
    }

private:
    webfront::test::TemporaryDirectory root;
};

string readAll(fs::File& file) {
    string           content;
    array<char, 512> buffer;
    while (auto bytesRead = file.read(buffer))
        content.append(buffer.data(), bytesRead);
    return content;
}

}  // namespace

// The default composition is the protected bridge, the optional root and IndexFS, nothing else.
static_assert(std::is_same_v<fs::DefaultFS, fs::Multi<fs::detail::ProtectedWebFrontJsFS, fs::detail::OptionalDevRootFS, fs::IndexFS>>);

const Scenario defaultFSServes{"DefaultFS serves only embedded assets when no document root is configured", [] {
                                   given("a DefaultFS constructed with an empty document root", [&] {
                                       fs::DefaultFS defaultFS{filesystem::path{}};

                                       then("the embedded WebFront.js, index.html, and favicon.ico are served", [&] {
                                           require(defaultFS.open("WebFront.js").has_value());
                                           require(defaultFS.open("index.html").has_value());
                                           require(defaultFS.open("favicon.ico").has_value());
                                       });

                                       then("an arbitrary file is not found, and no local filesystem access is attempted", [&] {
                                           require(!defaultFS.open("does-not-exist.txt").has_value());
                                           require(!defaultFS.open("webfront-bootstrap.mjs").has_value());
                                       });
                                   });
                               }};

const Scenario defaultFSLayers{"DefaultFS layers a configured document root over the embedded fallback", [] {
                                   given("a document root containing a custom index.html", [&] {
                                       TemporaryRoot root;
                                       root.write("index.html", "<!DOCTYPE html><title>Custom</title>");
                                       fs::DefaultFS defaultFS{root.path()};

                                       when("index.html is requested", [&] {
                                           auto file = defaultFS.open("index.html");
                                           then("the custom file overrides the embedded fallback page", [&] {
                                               require(file.has_value());
                                               require(readAll(*file) == "<!DOCTYPE html><title>Custom</title>");
                                           });
                                       });
                                   });

                                   given("a document root that also contains a file named WebFront.js", [&] {
                                       TemporaryRoot root;
                                       root.write("WebFront.js", "// not the real bridge script");
                                       root.write("webfront-dom.mjs", "// wrong DOM version");
                                       fs::DefaultFS defaultFS{root.path()};

                                       when("WebFront.js is requested", [&] {
                                           auto file = defaultFS.open("WebFront.js");
                                           then("the embedded, version-matched script is served instead of the local file", [&] {
                                               require(file.has_value());
                                               require(readAll(*file) != "// not the real bridge script");
                                               auto dom = defaultFS.open("webfront-dom.mjs");
                                               require(dom.has_value());
                                               require(readAll(*dom) != "// wrong DOM version");
                                           });
                                       });
                                   });

                                   given("a document root containing a file not otherwise served", [&] {
                                       TemporaryRoot root;
                                       root.write("app.mjs", "export const ready = true;");
                                       fs::DefaultFS defaultFS{root.path()};

                                       when("app.mjs is requested", [&] {
                                           auto file = defaultFS.open("app.mjs");
                                           then("the local development file is served", [&] {
                                               require(file.has_value());
                                               require(readAll(*file) == "export const ready = true;");
                                           });
                                       });
                                   });
                               }};
