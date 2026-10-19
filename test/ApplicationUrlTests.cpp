#include <catch2/catch_test_macros.hpp>
#include <frontend/Url.hpp>

SCENARIO("Frontend URLs use the configured listening address") {
    using webfront::frontend::detail::applicationUrl;
    THEN("IPv4 and IPv6 addresses are preserved and wildcard binds open loopback") {
        REQUIRE(applicationUrl("127.0.0.2", "9010", "index.html") == "http://127.0.0.2:9010/index.html");
        REQUIRE(applicationUrl("::1", "9010", "index.html") == "http://[::1]:9010/index.html");
        REQUIRE(applicationUrl("[::1]", "9010", "index.html") == "http://[::1]:9010/index.html");
        REQUIRE(applicationUrl("0.0.0.0", "9010", "index.html") == "http://127.0.0.1:9010/index.html");
        REQUIRE(applicationUrl("::", "9010", "index.html") == "http://[::1]:9010/index.html");
        REQUIRE(applicationUrl("localhost", "9010", "index.html") == "http://localhost:9010/index.html");
    }
    THEN("addresses and ports cannot inject URL authority or shell syntax") {
        REQUIRE_THROWS_AS(applicationUrl("", "9010", "index.html"), std::invalid_argument);
        REQUIRE_THROWS_AS(applicationUrl("127.0.0.1/evil", "9010", "index.html"), std::invalid_argument);
        REQUIRE_THROWS_AS(applicationUrl("127.0.0.1", "", "index.html"), std::invalid_argument);
        REQUIRE_THROWS_AS(applicationUrl("127.0.0.1", "90'10", "index.html"), std::invalid_argument);
    }
}

SCENARIO("Frontend URLs preserve query and fragment syntax consistently") {
    using webfront::frontend::detail::applicationUrl;
    THEN("spaces and UTF-8 bytes are encoded without escaping URI delimiters") {
        REQUIRE(applicationUrl("127.0.0.1", "9002", "folder/a b.html?x=1&name=Ren\xc3\xa9#result")
                == "http://127.0.0.1:9002/folder/a%20b.html?x=1&name=Ren%C3%A9#result");
    }
    THEN("literal delimiters use percent escapes which are not encoded twice") {
        REQUIRE(applicationUrl("127.0.0.1", "9002", "a%23b%3Fc%25.html") == "http://127.0.0.1:9002/a%23b%3Fc%25.html");
        REQUIRE(applicationUrl("127.0.0.1", "9002", "bad%xy%.html") == "http://127.0.0.1:9002/bad%25xy%25.html");
        REQUIRE(applicationUrl("127.0.0.1", "9002", "a'b.html") == "http://127.0.0.1:9002/a%27b.html");
    }
}
