// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-07-22
//
// WebFront — tests/unit
// Role: The default file system: precedence of the embedded script, the document root, an embedded interface and the fallback page.
// Thread: bridge-io | Alloc: heap-init
// LLM: uses temporary roots

#include "TemporaryDirectory.hpp"
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

namespace {

// An application's embedded interface, as webfront_embed_directory() or wf embed generates it.
class EmbeddedInterface {
public:
    explicit EmbeddedInterface(filesystem::path) { }
    static optional<fs::File> open(filesystem::path path) {
        static constexpr array<uint64_t, 1> index{0x656d626564646564};   // "embedded"
        static constexpr array<uint64_t, 1> script{0x7363726970740000};  // "script"
        const auto                          name = path.relative_path().generic_string();
        if (name == "index.html")
            return fs::File{index, 8};
        if (name == "app.js" || name == "WebFront.js")
            return fs::File{script, 6};
        return {};
    }
};

}  // namespace

static_assert(std::is_same_v<fs::DocumentRootFirstFSWith<EmbeddedInterface>,
                             fs::Multi<fs::detail::ProtectedWebFrontJsFS, fs::detail::OptionalDevRootFS, EmbeddedInterface, fs::IndexFS>>);

const Scenario documentRootFirst{"DocumentRootFirstFSWith serves a configured document root in front of an embedded interface", [] {
                                     given("no document root", [] {
                                         fs::DocumentRootFirstFSWith<EmbeddedInterface> provider{filesystem::path{}};
                                         then("the embedded interface replaces the fallback page, and the bridge stays protected", [&] {
                                             auto index = provider.open("index.html");
                                             require(index.has_value());
                                             requireEqual(readAll(*index), string{"embedded"});
                                             auto script = provider.open("app.js");
                                             require(script.has_value());
                                             requireEqual(readAll(*script), string{"script"});
                                             auto bridge = provider.open("WebFront.js");
                                             require(bridge.has_value());
                                             require(readAll(*bridge) != "script");
                                             require(provider.open("favicon.ico").has_value());
                                         });
                                     });
                                     given("a document root holding a newer index.html", [] {
                                         TemporaryRoot root;
                                         root.write("index.html", "on disk");
                                         root.write("WebFront.js", "// not the real bridge script");
                                         fs::DocumentRootFirstFSWith<EmbeddedInterface> provider{root.path()};
                                         then("the root's file wins, the embedded one serves what the root lacks, and WebFront.js is never shadowed", [&] {
                                             auto index = provider.open("index.html");
                                             require(index.has_value());
                                             requireEqual(readAll(*index), string{"on disk"});
                                             auto script = provider.open("app.js");
                                             require(script.has_value());
                                             requireEqual(readAll(*script), string{"script"});
                                             auto bridge = provider.open("WebFront.js");
                                             require(bridge.has_value());
                                             const auto bridgeText = readAll(*bridge);
                                             require(bridgeText != "script" && bridgeText != "// not the real bridge script");
                                         });
                                     });
                                 }};
