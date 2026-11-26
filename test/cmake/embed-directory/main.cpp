// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-26
//
// WebFront — tests/cmake
// Role: Consumer of webfront_embed_directory(): serves every file of a directory from the embedded provider and from the disk, and compares them.
// Thread: ui-loop | Alloc: unrestricted
// LLM: run by CheckEmbedDirectory.cmake, which changes the directory between builds; a stale provider must fail here

#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/system/NativeFS.hpp>

#ifdef EMBED_DEFAULT_NAMES
    #include <EmbeddedDirectory.hpp>
using Embedded = webfront::fs::EmbeddedDirectory;
#else
    #include <ConsumerUI.hpp>
using Embedded = consumer::ui::ConsumerUI;
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace webfront;
using Net = networking::NetworkingMock;

namespace {

struct Served {
    http::Response::StatusCode status;
    std::optional<std::string> type;
    std::string                content;
};

// A request line names the path percent-encoded where a URL must (a space); the server decodes it.
std::string target(std::string_view path) {
    std::string encoded = "/";
    for (const char c : path)
        encoded += c == ' ' ? std::string{"%20"} : std::string{c};
    return encoded;
}

template <typename Handler>
Served serve(Handler& handler, const std::string& uri) {
    const std::string text = "GET " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    http::Request     request;
    request.parseSomeData(text.cbegin(), text.cend());
    auto response = handler.handleRequest(request);
    auto type     = response.getHeaderValue("Content-Type");
    return {response.statusCode, type ? std::optional<std::string>{std::string{*type}} : std::nullopt, response.content};
}

std::string bytesOf(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

// embed_consumer <directory> [--absent <path>]...: every file of the directory, but its hidden ones, is served with the same
// status, type and bytes by the application's composition (no document root) and by the disk; each absent path and each
// hidden file is a 404 from the embedded provider.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: embed_consumer <directory> [--absent <path>]...\n";
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    std::vector<std::string>    absent;
    for (int index = 2; index + 1 < argc; index += 2)
        if (std::string_view{argv[index]} == "--absent")
            absent.emplace_back(argv[index + 1]);

    http::RequestHandler<Net, fs::DefaultFSWith<Embedded>> embedded{std::filesystem::path{}};
    http::RequestHandler<Net, fs::NativeDebugFS>           disk{directory};
    int                                                    failures = 0, files = 0;
    const auto                                             fail = [&](const std::string& what) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        const auto path         = std::filesystem::relative(entry.path(), directory).generic_string();
        const bool hidden       = path.starts_with('.') || path.find("/.") != std::string::npos;
        const auto fromEmbedded = serve(embedded, target(path));
        if (hidden) {
            if (fromEmbedded.status != http::Response::notFound)
                fail(path + ": an excluded file is served");
            continue;
        }
        ++files;
        const auto fromDisk = serve(disk, target(path));
        if (fromEmbedded.status != http::Response::ok || fromDisk.status != http::Response::ok)
            fail(path + ": status " + std::to_string(fromEmbedded.status) + " embedded, " + std::to_string(fromDisk.status) + " on disk");
        if (fromEmbedded.type != fromDisk.type || !fromEmbedded.type)
            fail(path + ": type " + fromEmbedded.type.value_or("none") + " embedded, " + fromDisk.type.value_or("none") + " on disk");
        if (fromEmbedded.content != fromDisk.content || fromEmbedded.content != bytesOf(entry.path()))
            fail(path + ": " + std::to_string(fromEmbedded.content.size()) + " bytes embedded differ from the " + std::to_string(fromDisk.content.size())
                 + " bytes on disk");
    }
    // The root of the site is its index.html, as from a document root.
    if (std::filesystem::exists(directory / "index.html") && serve(embedded, "/").content != bytesOf(directory / "index.html"))
        fail("/ does not serve index.html");
    for (const auto& path : absent)
        if (serve(embedded, target(path)).status != http::Response::notFound)
            fail(path + ": a removed file is still served");
    std::cout << files << " files served identically, " << absent.size() << " absent paths not found, " << failures << " failures\n";
    return failures == 0 && files > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
