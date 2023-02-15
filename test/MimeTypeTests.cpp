#include <http/MimeType.hpp>

#include <doctest/doctest.h>

#include <string>

using namespace webfront::http;

SCENARIO("MimeTypes") {
    REQUIRE(MimeType::fromExtension("jpg").type == MimeType::jpg);
    REQUIRE(MimeType::fromExtension("js").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("mjs").type == MimeType::js);

    REQUIRE(std::string(MimeType::fromExtension("html").toString()) == "text/html");
    REQUIRE(std::string(MimeType::fromExtension("htm").toString()) == "text/html");
    REQUIRE(std::string(MimeType::fromExtension("bozo").toString()) == "text/plain");
    REQUIRE(std::string(MimeType::fromExtension("gif").toString()) == "image/gif");

    REQUIRE(std::string(MimeType("pdf").toString()) == "application/pdf");
    REQUIRE(std::string(MimeType(".ico").toString()) == "image/x-icon");
    REQUIRE(std::string(MimeType(".webp").toString()) == "image/webp");
    REQUIRE(std::string(MimeType("webp").toString()) == "image/webp");
}