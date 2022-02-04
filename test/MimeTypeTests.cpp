#include <http/MimeType.hpp>

#include <catch2/catch.hpp>

using namespace webfront::http;

SCENARIO("MimeTypes") {
    REQUIRE(MimeType::fromExtension("jpg").type == MimeType::jpg);
    REQUIRE(MimeType::fromExtension("js").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("mjs").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("html").toString() == "text/html");
    REQUIRE(MimeType::fromExtension("htm").toString() == "text/html");
    REQUIRE(MimeType::fromExtension("bozo").toString() == "text/plain");
    REQUIRE(MimeType::fromExtension("gif").toString() == "image/gif");

    REQUIRE(MimeType("pdf").toString() == "application/pdf");
    REQUIRE(MimeType(".ico").toString() == "image/x-icon");
    REQUIRE(MimeType(".webp").toString() == "image/webp");
    REQUIRE(MimeType("webp").toString() == "image/webp");
}