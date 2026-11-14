#include "Bdd.hpp"
#include <webfront/http/HTTPServer.hpp>

using namespace webfront::bdd;

using webfront::http::Headers;

Scenario acceptEncodingDefaults{"Accept-Encoding defaults to the identity fallback", [] {
    for (const auto includeEmptyField : {false, true}) {
        given("an absent or empty field: ", includeEmptyField, [&] {
            Headers headers;
            if (includeEmptyField)
                headers.headers.emplace_back("Accept-Encoding", "");
            then("identity is available and gzip is not advertised", [&] {
                require(headers.acceptsEncoding("identity"));
                require(!headers.acceptsEncoding("gzip"));
            });
        });
    }
}};

Scenario acceptEncodingMatches{"Accept-Encoding matches complete case-insensitive tokens with HTTP whitespace", [] {
    for (const auto value : {"gzip", "GZIP", " GZIP \t", "br,\tgzip ; q = 0.5 ", "gzip;q=1.000", ", gzip,,"}) {
        given(value, [&] {
            Headers headers;
            headers.headers.emplace_back("accept-ENCODING", value);
            then("gzip is accepted", [&] {
                require(headers.acceptsEncoding("gzip"));
            });
        });
    }
    for (const auto value : {"xgzip", "gzip2", "br, deflate"}) {
        given(value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            then("substrings do not advertise gzip", [&] {
                require(!headers.acceptsEncoding("gzip"));
            });
        });
    }
}};

Scenario acceptEncodingRespects{"Accept-Encoding respects explicit and wildcard exclusions", [] {
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
        given(preference.value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", preference.value);
            then("explicit tokens override the wildcard", [&] {
                require(headers.acceptsEncoding("gzip") == preference.gzip);
                require(headers.acceptsEncoding("identity") == preference.identity);
            });
        });
    }
}};

Scenario acceptEncodingRejects{"Accept-Encoding rejects malformed qvalues", [] {
    for (const auto value : {"1.5", "", "invalid", "-1", "01", "0.0001", "1.001", ".5"}) {
        given("q=", value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", std::string("gzip;q=") + value);
            then("the malformed preference does not accept gzip", [&] {
                require(!headers.acceptsEncoding("gzip"));
            });
        });
    }
}};

Scenario acceptEncodingIgnores{"Accept-Encoding ignores unknown parameters while preserving q exclusions", [] {
    for (const auto value : {"gzip;level=1", "gzip;level=1;q=0.5", "gzip; q=1; level=1", "gzip;extension"}) {
        given(value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            then("extension parameters do not disable gzip", [&] {
                require(headers.acceptsEncoding("gzip"));
            });
        });
    }
    for (const auto value : {"gzip;level=1;q=0", "gzip;q=0;level=1", "gzip;level=1;q=1.5"}) {
        given(value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            then("extensions cannot override a rejected qvalue", [&] {
                require(!headers.acceptsEncoding("gzip"));
            });
        });
    }
}};

Scenario acceptEncodingCombines{"Accept-Encoding combines multiple fields before applying wildcard precedence", [] {
    Headers headers;
    headers.headers.emplace_back("Accept-Encoding", "*;q=0");
    headers.headers.emplace_back("accept-encoding", "GZIP;q=0.5");
    headers.headers.emplace_back("ACCEPT-ENCODING", "identity;q=1");
    then("an explicit preference in any field overrides the wildcard", [&] {
        require(headers.acceptsEncoding("gzip"));
        require(headers.acceptsEncoding("identity"));
        require(!headers.acceptsEncoding("br"));
    });
    given("a wildcard acceptance and an explicit gzip exclusion in separate fields", [&] {
        headers.headers.clear();
        headers.headers.emplace_back("Accept-Encoding", "*;q=1");
        headers.headers.emplace_back("accept-encoding", "gzip;q=0");
        then("the explicit exclusion still wins", [&] {
            require(!headers.acceptsEncoding("gzip"));
            require(headers.acceptsEncoding("br"));
        });
    });
}};

Scenario acceptEncodingAccepts{"Accept-Encoding accepts any positive duplicate regardless of order", [] {
    for (const auto value : {"gzip;q=0, gzip", "gzip, gzip;q=0", "gzip;q=.5, gzip", "gzip, gzip;q=.5"}) {
        given(value, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", value);
            then("a valid accepting mention wins over the rejected mention", [&] {
                require(headers.acceptsEncoding("gzip"));
            });
        });
    }
    for (const auto first : {"gzip", "gzip;q=0"}) {
        given("first field: ", first, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", first);
            headers.headers.emplace_back("Accept-Encoding", first == std::string_view("gzip") ? "gzip;q=0" : "gzip");
            then("duplicate mentions across fields follow the same rule", [&] {
                require(headers.acceptsEncoding("gzip"));
            });
        });
    }
}};

static_assert(webfront::http::encoding::tokenEquals("GZIP", "gzip"));
static_assert(!webfront::http::encoding::tokenEquals("\xc0", "\xe0"));

Scenario hTTPEncoding{"HTTP encoding token comparison folds ASCII letters only", [] {
    given("ASCII punctuation", [&] {
        then("case folding does not conflate punctuation pairs", [&] {
            require(!webfront::http::encoding::tokenEquals("@", "`"));
            require(!webfront::http::encoding::tokenEquals("[", "{"));
        });
    });
    given("every non-ASCII byte", [&] {
        then("each byte compares equal only to itself among its bit-5 pair", [&] {
            for (unsigned value = 128; value < 256; ++value) {
                const std::string original(1, static_cast<char>(value));
                const std::string other(1, static_cast<char>(value ^ 0x20));
                require(webfront::http::encoding::tokenEquals(original, original));
                require(!webfront::http::encoding::tokenEquals(original, other));
            }
        });
    });
}};

Scenario acceptEncodingTreats{"Accept-Encoding treats x-gzip as the gzip alias", [] {
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
        given(preference.field, [&] {
            Headers headers;
            headers.headers.emplace_back("Accept-Encoding", preference.field);
            then("both names share the explicit preference and wildcard precedence", [&] {
                require(headers.acceptsEncoding("gzip") == preference.accepted);
                require(headers.acceptsEncoding("x-gzip") == preference.accepted);
            });
        });
    }
    given("the aliases in separate fields", [&] {
        Headers headers;
        headers.headers.emplace_back("Accept-Encoding", "gzip;q=0");
        headers.headers.emplace_back("Accept-Encoding", "x-gzip");
        then("accepting duplicates merge across aliases and fields", [&] {
            require(headers.acceptsEncoding("gzip"));
        });
    });
}};
