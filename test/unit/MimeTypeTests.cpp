#include "Bdd.hpp"
#include <webfront/http/MimeType.hpp>

#include <string>

using namespace webfront::bdd;

using namespace webfront::http;

const Scenario mimeTypes{"MimeTypes", [] {
    require(MimeType::fromExtension("jpg").type == MimeType::jpg);
    require(MimeType::fromExtension("js").type == MimeType::js);
    require(MimeType::fromExtension("mjs").type == MimeType::js);
    require(MimeType::fromExtension("css").type == MimeType::css);
    require(MimeType::fromExtension("png").type == MimeType::png);
    require(MimeType::fromExtension("json").type == MimeType::json);
    require(MimeType::fromExtension("ttf").type == MimeType::ttf);
    require(MimeType::fromExtension("svg").type == MimeType::svg);
    require(MimeType::fromExtension("csv").type == MimeType::csv);

    require(std::string(MimeType::fromExtension("html").toString()) == "text/html");
    require(std::string(MimeType::fromExtension("htm").toString()) == "text/html");
    require(std::string(MimeType::fromExtension("bozo").toString()) == "text/plain");
    require(std::string(MimeType::fromExtension("gif").toString()) == "image/gif");

    require(std::string(MimeType("pdf").toString()) == "application/pdf");
    require(std::string(MimeType(".ico").toString()) == "image/x-icon");
    require(std::string(MimeType(".webp").toString()) == "image/webp");
    require(std::string(MimeType("webp").toString()) == "image/webp");
}};
const Scenario everyMIME{"Every MIME type has its own name", [] {
    given("each extension and the content type a browser expects for it", [&] {
        const std::pair<std::string, std::string> types[]{
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
        };
        for (const auto& [extension, contentType] : types)
            then(extension + " is served as " + contentType, [&] {
                require(std::string(MimeType::fromExtension(extension).toString()) == contentType);
            });
    });
}};
