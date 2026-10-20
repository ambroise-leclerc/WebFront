#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <http/MimeType.hpp>

#include <string>

using namespace webfront::http;

SCENARIO("MimeTypes") {
    REQUIRE(MimeType::fromExtension("jpg").type == MimeType::jpg);
    REQUIRE(MimeType::fromExtension("js").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("mjs").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("css").type == MimeType::css);
    REQUIRE(MimeType::fromExtension("png").type == MimeType::png);
    REQUIRE(MimeType::fromExtension("json").type == MimeType::json);
    REQUIRE(MimeType::fromExtension("ttf").type == MimeType::ttf);
    REQUIRE(MimeType::fromExtension("svg").type == MimeType::svg);
    REQUIRE(MimeType::fromExtension("csv").type == MimeType::csv);

    REQUIRE(std::string(MimeType::fromExtension("html").toString()) == "text/html");
    REQUIRE(std::string(MimeType::fromExtension("htm").toString()) == "text/html");
    REQUIRE(std::string(MimeType::fromExtension("bozo").toString()) == "text/plain");
    REQUIRE(std::string(MimeType::fromExtension("gif").toString()) == "image/gif");

    REQUIRE(std::string(MimeType("pdf").toString()) == "application/pdf");
    REQUIRE(std::string(MimeType(".ico").toString()) == "image/x-icon");
    REQUIRE(std::string(MimeType(".webp").toString()) == "image/webp");
    REQUIRE(std::string(MimeType("webp").toString()) == "image/webp");
}
SCENARIO("Every MIME type has its own name") {
    GIVEN("each extension and the content type a browser expects for it") {
        auto [extension, contentType] = GENERATE(table<std::string, std::string>({
            { "txt",             "text/plain"},
            {"html",              "text/html"},
            { "css",               "text/css"},
            {  "js", "application/javascript"},
            { "jpg",             "image/jpeg"},
            { "png",              "image/png"},
            { "gif",              "image/gif"},
            {"json",       "application/json"},
            { "pdf",        "application/pdf"},
            { "ttf",               "font/ttf"},
            { "ico",           "image/x-icon"},
            { "svg",          "image/svg+xml"},
            {"webp",             "image/webp"},
            { "csv",               "text/csv"},
            {"wasm",       "application/wasm"},
        }));
        THEN(extension + " is served as " + contentType) {
            REQUIRE(std::string(MimeType::fromExtension(extension).toString()) == contentType);
        }
    }
}
