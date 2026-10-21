#include <catch2/catch_test_macros.hpp>
#include <http/HTTPServer.hpp>

using webfront::http::Headers;

SCENARIO("Accept-Encoding defaults to the identity fallback") {
    for (const auto includeEmptyField : {false, true}) {
        GIVEN("an absent or empty field: " << includeEmptyField) {
            Headers headers;
            if (includeEmptyField)
                headers.headers.emplace_back("Accept-Encoding", "");
            THEN("identity is available and gzip is not advertised") {
                REQUIRE(headers.acceptsEncoding("identity"));
                REQUIRE_FALSE(headers.acceptsEncoding("gzip"));
            }
        }
    }
}

SCENARIO("Accept-Encoding matches complete case-insensitive tokens with HTTP whitespace") {
    for (const auto value : {"gzip", "GZIP", " GZIP \t", "br,\tgzip ; q = 0.5 ", "gzip;q=1.000", ", gzip,,"}) {
        GIVEN(value) {
            Headers headers;
            headers.headers.emplace_back("accept-ENCODING", value);
            THEN("gzip is accepted") {
                REQUIRE(headers.acceptsEncoding("gzip"));
            }
        }
    }
    for (const auto value : {"xgzip", "gzip2", "br, deflate"}) {
        GIVEN(value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            THEN("substrings do not advertise gzip") {
                REQUIRE_FALSE(headers.acceptsEncoding("gzip"));
            }
        }
    }
}

SCENARIO("Accept-Encoding respects explicit and wildcard exclusions") {
    struct Preference {
        const char* value;
        bool        gzip;
        bool        identity;
    };
    for (const auto& preference : {
             Preference{             "gzip;q=0", false,  true},
             {                "*;q=0", false, false},
             {         "identity;q=0", false, false},
             {      "gzip;q=0, *;q=1", false,  true},
             {      "*;q=0, gzip;q=1",  true, false},
             {"*;q=0, identity;q=0.5", false,  true},
             {   "gzip, identity;q=0",  true, false}
    }) {
        GIVEN(preference.value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", preference.value);
            THEN("explicit tokens override the wildcard") {
                REQUIRE(headers.acceptsEncoding("gzip") == preference.gzip);
                REQUIRE(headers.acceptsEncoding("identity") == preference.identity);
            }
        }
    }
}

SCENARIO("Accept-Encoding rejects malformed qvalues") {
    for (const auto value : {"1.5", "", "invalid", "-1", "01", "0.0001", "1.001", ".5"}) {
        GIVEN("q=" << value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", std::string("gzip;q=") + value);
            THEN("the malformed preference does not accept gzip") {
                REQUIRE_FALSE(headers.acceptsEncoding("gzip"));
            }
        }
    }
}

SCENARIO("Accept-Encoding ignores unknown parameters while preserving q exclusions") {
    for (const auto value : {"gzip;level=1", "gzip;level=1;q=0.5", "gzip; q=1; level=1", "gzip;extension"}) {
        GIVEN(value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            THEN("extension parameters do not disable gzip") {
                REQUIRE(headers.acceptsEncoding("gzip"));
            }
        }
    }
    for (const auto value : {"gzip;level=1;q=0", "gzip;q=0;level=1", "gzip;level=1;q=1.5"}) {
        GIVEN(value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            THEN("extensions cannot override a rejected qvalue") {
                REQUIRE_FALSE(headers.acceptsEncoding("gzip"));
            }
        }
    }
}

SCENARIO("Accept-Encoding combines multiple fields before applying wildcard precedence") {
    Headers headers;
    headers.headers.emplace_back("Accept-Encoding", "*;q=0");
    headers.headers.emplace_back("accept-encoding", "GZIP;q=0.5");
    headers.headers.emplace_back("ACCEPT-ENCODING", "identity;q=1");
    THEN("an explicit preference in any field overrides the wildcard") {
        REQUIRE(headers.acceptsEncoding("gzip"));
        REQUIRE(headers.acceptsEncoding("identity"));
        REQUIRE_FALSE(headers.acceptsEncoding("br"));
    }
    GIVEN("a wildcard acceptance and an explicit gzip exclusion in separate fields") {
        headers.headers.clear();
        headers.headers.emplace_back("Accept-Encoding", "*;q=1");
        headers.headers.emplace_back("accept-encoding", "gzip;q=0");
        THEN("the explicit exclusion still wins") {
            REQUIRE_FALSE(headers.acceptsEncoding("gzip"));
            REQUIRE(headers.acceptsEncoding("br"));
        }
    }
}

SCENARIO("Accept-Encoding accepts any positive duplicate regardless of order") {
    for (const auto value : {"gzip;q=0, gzip", "gzip, gzip;q=0", "gzip;q=.5, gzip", "gzip, gzip;q=.5"}) {
        GIVEN(value) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            THEN("a valid accepting mention wins over the rejected mention") {
                REQUIRE(headers.acceptsEncoding("gzip"));
            }
        }
    }
    for (const auto first : {"gzip", "gzip;q=0"}) {
        GIVEN("first field: " << first) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", first);
            headers.headers.emplace_back("Accept-Encoding", first == std::string_view("gzip") ? "gzip;q=0" : "gzip");
            THEN("duplicate mentions across fields follow the same rule") {
                REQUIRE(headers.acceptsEncoding("gzip"));
            }
        }
    }
}

static_assert(webfront::http::encoding::tokenEquals("GZIP", "gzip"));
static_assert(!webfront::http::encoding::tokenEquals("\xc0", "\xe0"));

SCENARIO("HTTP encoding token comparison folds ASCII letters only") {
    GIVEN("ASCII punctuation") {
        THEN("case folding does not conflate punctuation pairs") {
            REQUIRE_FALSE(webfront::http::encoding::tokenEquals("@", "`"));
            REQUIRE_FALSE(webfront::http::encoding::tokenEquals("[", "{"));
        }
    }
    GIVEN("every non-ASCII byte") {
        THEN("each byte compares equal only to itself among its bit-5 pair") {
            for (unsigned value = 128; value < 256; ++value) {
                const std::string original(1, static_cast<char>(value));
                const std::string other(1, static_cast<char>(value ^ 0x20));
                REQUIRE(webfront::http::encoding::tokenEquals(original, original));
                REQUIRE_FALSE(webfront::http::encoding::tokenEquals(original, other));
            }
        }
    }
}

SCENARIO("Accept-Encoding treats x-gzip as the gzip alias") {
    struct Preference {
        const char* field;
        bool        accepted;
    };
    for (const auto& preference : {
             Preference{           "x-gzip",  true},
             {     "X-GZIP;q=0.5",  true},
             {       "x-gzip;q=0", false},
             {"*;q=1, x-gzip;q=0", false},
             {    "*;q=0, x-gzip",  true},
             { "gzip;q=0, x-gzip",  true},
             { "x-gzip;q=0, gzip",  true},
             {      "x-gzip;q=.5", false},
             {          "x-gzip2", false}
    }) {
        GIVEN(preference.field) {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", preference.field);
            THEN("both names share the explicit preference and wildcard precedence") {
                REQUIRE(headers.acceptsEncoding("gzip") == preference.accepted);
                REQUIRE(headers.acceptsEncoding("x-gzip") == preference.accepted);
            }
        }
    }
    GIVEN("the aliases in separate fields") {
        Headers headers;
        headers.headers.emplace_back("Accept-Encoding", "gzip;q=0");
        headers.headers.emplace_back("Accept-Encoding", "x-gzip");
        THEN("accepting duplicates merge across aliases and fields") {
            REQUIRE(headers.acceptsEncoding("gzip"));
        }
    }
}
