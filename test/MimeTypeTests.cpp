#include <http/MimeType.hpp>

#include <doctest/doctest.h>
using namespace webfront::http;
using namespace std::literals;

SCENARIO("MimeTypes") {
    REQUIRE(MimeType::fromExtension("jpg").type == MimeType::jpg);
    REQUIRE(MimeType::fromExtension("js").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("mjs").type == MimeType::js);

    REQUIRE(std::string(MimeType::fromExtension("html").toString()) == "text/html");
    REQUIRE(std::string(MimeType::fromExtension("htm").toString()) == "text/html"s);
    REQUIRE(std::string(MimeType::fromExtension("bozo").toString()) == "text/plain"s);
    REQUIRE(std::string(MimeType::fromExtension("gif").toString()) == "image/gif"s);

    REQUIRE(std::string(MimeType("pdf").toString()) == "application/pdf"s);
    REQUIRE(std::string(MimeType(".ico").toString()) == "image/x-icon"s);
    REQUIRE(std::string(MimeType(".webp").toString()) == "image/webp"s);
    REQUIRE(std::string(MimeType("webp").toString()) == "image/webp"s);
}