#include <http/MimeType.hpp>

#include <doctest/doctest.h>
using namespace webfront::http;
using namespace std::literals;

SCENARIO("MimeTypes") {
    REQUIRE(MimeType::fromExtension("jpg").type == MimeType::jpg);
    REQUIRE(MimeType::fromExtension("js").type == MimeType::js);
    REQUIRE(MimeType::fromExtension("mjs").type == MimeType::js);
    /* REQUIRE(MimeType::fromExtension("html").toString() == "text/html"s);
    REQUIRE(MimeType::fromExtension("htm").toString() == "text/html"s);
    REQUIRE(MimeType::fromExtension("bozo").toString() == "text/plain"s);
    REQUIRE(MimeType::fromExtension("gif").toString() == "image/gif"s);

    REQUIRE(MimeType("pdf").toString() == "application/pdf"s);
    REQUIRE(MimeType(".ico").toString() == "image/x-icon"s);
    REQUIRE(MimeType(".webp").toString() == "image/webp"s);
    REQUIRE(MimeType("webp").toString() == "image/webp"s);
*/}