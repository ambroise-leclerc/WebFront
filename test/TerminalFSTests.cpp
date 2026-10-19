#include <catch2/catch_test_macros.hpp>
#include <http/MimeType.hpp>
#include <system/DefaultFS.hpp>
#include <system/TerminalFS.hpp>

#include <array>
#include <filesystem>
#include <string>

using namespace std;
using namespace webfront;

namespace {

string readAll(fs::File& file) {
    string            content;
    array<char, 4096> buffer;
    while (auto bytesRead = file.read(buffer))
        content.append(buffer.data(), bytesRead);
    return content;
}

string read(auto& filesystem, const string& path) {
    auto file = filesystem.open(path);
    REQUIRE(file.has_value());
    REQUIRE_FALSE(file->isEncoded());
    return readAll(*file);
}

}  // namespace

SCENARIO("TerminalFS serves the terminal component under its reserved prefix") {
    GIVEN("a TerminalFS") {
        fs::TerminalFS terminalFS{filesystem::path{}};
        const string   prefix{fs::TerminalFS::prefix};

        THEN("the page loads the bridge from the root and the component module next to it") {
            REQUIRE(prefix + "index.html" == fs::TerminalFS::page);
            const auto page = read(terminalFS, string{fs::TerminalFS::page});
            REQUIRE(page.find(R"(<script src="/WebFront.js"></script>)") != string::npos);
            REQUIRE(page.find(R"(from "./terminal.mjs")") != string::npos);
            REQUIRE(page.find(R"(href="xterm.css")") != string::npos);
        }
        THEN("the component module imports the vendored xterm.js modules") {
            const auto module = read(terminalFS, prefix + "terminal.mjs");
            REQUIRE(module.find(R"(from "./xterm.mjs")") != string::npos);
            REQUIRE(module.find(R"(from "./addon-fit.mjs")") != string::npos);
        }
        THEN("the vendored xterm.js files keep their license notice and exports") {
            const auto xterm = read(terminalFS, prefix + "xterm.mjs");
            REQUIRE(xterm.find("@license MIT") != string::npos);
            REQUIRE(xterm.find("as Terminal}") != string::npos);
            REQUIRE(read(terminalFS, prefix + "addon-fit.mjs").find("as FitAddon}") != string::npos);
            REQUIRE(read(terminalFS, prefix + "xterm.css").find(".xterm") != string::npos);
        }
        THEN("each served file has the MIME type the browser needs for modules and styles") {
            auto mime = [](const string& path) {
                return http::MimeType::fromExtension(filesystem::path{path}.extension().string()).toString();
            };
            REQUIRE(mime(prefix + "index.html") == "text/html");
            REQUIRE(mime(prefix + "terminal.mjs") == "application/javascript");
            REQUIRE(mime(prefix + "xterm.mjs") == "application/javascript");
            REQUIRE(mime(prefix + "xterm.css") == "text/css");
        }
        THEN("other paths, including the bare names outside the prefix, are not served") {
            REQUIRE_FALSE(terminalFS.open("xterm.mjs").has_value());
            REQUIRE_FALSE(terminalFS.open("index.html").has_value());
            REQUIRE_FALSE(terminalFS.open(prefix + "missing.mjs").has_value());
            REQUIRE_FALSE(terminalFS.open(prefix + "xterm.mjs.map").has_value());
        }
    }
}

SCENARIO("DefaultFSWith adds component assets to the default WebFront filesystem") {
    GIVEN("DefaultFSWith<TerminalFS> without a document root") {
        fs::DefaultFSWith<fs::TerminalFS> filesystem{std::filesystem::path{}};

        THEN("the terminal page, the protected bridge and the fallback page are all served") {
            REQUIRE(filesystem.open(string{fs::TerminalFS::page}).has_value());
            REQUIRE(read(filesystem, "WebFront.js").find("WebFrontBridge") != string::npos);
            REQUIRE(filesystem.open("index.html").has_value());
            REQUIRE_FALSE(filesystem.open("unknown.txt").has_value());
        }
    }
}
