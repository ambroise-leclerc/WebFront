#include <http/Encodings.hpp>

#include <doctest/doctest.h>

#include <string>

using namespace webfront;
using namespace std;

SCENARIO("URL encoding") { REQUIRE(uri::encode("mango & pineapple") == "mango%20%26%20pineapple"); }

SCENARIO("URI decoding") {
    REQUIRE(uri::decode("http://path%25name%2F%25%23%21%25") == "http://path%name/%#!%");
    REQUIRE(uri::decode("http://ABC12345zef") == "http://ABC12345zef");
    REQUIRE(uri::decode("%20%RQabcABV") == " %RQabcABV");
}

SCENARIO("URI parsing") {
    GIVEN("An URI with no userinfo nor fragment") {
        uri::URI u0("http://localhost:80/foo.html?&q=1:2:3");
        WHEN("decoded") {
            REQUIRE(u0.scheme == "http");
            REQUIRE(u0.userinfo.empty());
            REQUIRE(u0.host == "localhost");
            REQUIRE(u0.port == "80");
            REQUIRE(u0.path == "/foo.html");
            REQUIRE(u0.authority == "localhost:80");
            REQUIRE(u0.query == "&q=1:2:3");
            REQUIRE(u0.fragment.empty());
        }
    }

    GIVEN("A complete URI") {
        uri::URI u1("https://john.doe@www.example.com:123/forum/questions/?tag=networking&order=newest#top");
        WHEN("decoded") {
            REQUIRE(u1.scheme == "https");
            REQUIRE(u1.userinfo == "john.doe");
            REQUIRE(u1.host == "www.example.com");
            REQUIRE(u1.port == "123");
            REQUIRE(u1.path == "/forum/questions/");
            REQUIRE(u1.authority == "john.doe@www.example.com:123");
            REQUIRE(u1.query == "tag=networking&order=newest");
            REQUIRE(u1.fragment == "top");
        }
    }

    GIVEN("A function which returns an URI from a string") {
        auto parse = [](std::string text) { return uri::URI(text); };

        WHEN("parsing URI without userinfo") {
            auto u = parse("https://www.example.com:123/forum/questions/?tag=networking&order=newest#top");
            THEN("fields should be") {
                REQUIRE(u.scheme == "https");
                REQUIRE(u.userinfo.empty());
                REQUIRE(u.host == "www.example.com");
                REQUIRE(u.port == "123");
                REQUIRE(u.path == "/forum/questions/");
                REQUIRE(u.authority == "www.example.com:123");
                REQUIRE(u.query == "tag=networking&order=newest");
                REQUIRE(u.fragment == "top");
            }
        }

        WHEN("parsing URI without userinfo nor port") {
            auto u = parse("https://www.example.com/forum/questions/?tag=networking&order=newest#top");
            THEN("fields should be") {
                REQUIRE(u.scheme == "https");
                REQUIRE(u.userinfo.empty());
                REQUIRE(u.host == "www.example.com");
                REQUIRE(u.port.empty());
                REQUIRE(u.path == "/forum/questions/");
                REQUIRE(u.authority == "www.example.com");
                REQUIRE(u.query == "tag=networking&order=newest");
                REQUIRE(u.fragment == "top");
            }
        }

        WHEN("parsing URI without userinfo nor port or query") {
            auto u = parse("https://www.example.com/forum/questions/");
            THEN("fields should be") {
                REQUIRE(u.scheme == "https");
                REQUIRE(u.userinfo.empty());
                REQUIRE(u.host == "www.example.com");
                REQUIRE(u.port.empty());
                REQUIRE(u.path == "/forum/questions/");
                REQUIRE(u.authority == "www.example.com");
                REQUIRE(u.query.empty());
                REQUIRE(u.fragment.empty());
            }
        }
    }
}

SCENARIO("WebSocket Sec Key coding") {
    std::string key = "x3JJHMbDL1EzLkh9GBhXDw==";
    REQUIRE(base64::encodeInNetworkOrder(crypto::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) ==
            "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");

    key = "dGhlIHNhbXBsZSBub25jZQ==";
    REQUIRE(base64::encodeInNetworkOrder(crypto::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

SCENARIO("Base64 encoding") {
    REQUIRE(base64::encodeInNetworkOrder(crypto::sha1("The quick brown fox jumps over the lazy dog")) ==
            "L9ThxnotKPzthJ7hu3bnORuT6xI=");
    REQUIRE(base64::encodeInNetworkOrder(crypto::sha1("The quick brown fox jumps over the lazy cog")) ==
            "3p8sf9JeGzr60+haC9F9mxANtLM=");
    REQUIRE(base64::encodeInNetworkOrder(crypto::sha1("")) == "2jmj7l5rSw0yVb/vlWAYkK/YBwk=");
}

SCENARIO("SHA1 hashing") {
    REQUIRE(crypto::sha1String("The quick brown fox jumps over the lazy dog") == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    REQUIRE(crypto::sha1String("The quick brown fox jumps over the lazy cog") == "de9f2c7fd25e1b3afad3e85a0bd17d9b100db4b3");
    REQUIRE(crypto::sha1String("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}