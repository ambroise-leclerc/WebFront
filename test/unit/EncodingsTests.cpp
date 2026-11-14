#include <webfront/http/Encodings.hpp>

#include "Bdd.hpp"

#include <string>

using namespace webfront::bdd;

using namespace webfront;
using namespace std;

Scenario uRLEncoding{"URL encoding", [] {
    require(uri::encode("mango & pineapple") == "mango%20%26%20pineapple");
    require(uri::encode("AZaz09-_.~") == "AZaz09-_.~");

    given("non-ASCII bytes", [&] {
        then("each byte is percent-encoded", [&] {
            require(uri::encode("\xC3\xA9") == "%C3%A9");  // é in UTF-8
            require(uri::encode("caf\xC3\xA9 \xE2\x82\xAC") == "caf%C3%A9%20%E2%82%AC");
            require(uri::encode(string_view("\x00\x7F\x80\xFF", 4)) == "%00%7F%80%FF");
        });
    });

    given("every byte value", [&] {
        string all;
        for (int byte = 0; byte < 256; ++byte)
            all += static_cast<char>(byte);
        then("decoding the encoded string restores it", [&] {
            require(uri::decode(uri::encode(all)) == all);
        });
    });
}};

Scenario uRIDecoding{"URI decoding", [] {
    require(uri::decode("http://path%25name%2F%25%23%21%25") == "http://path%name/%#!%");
    require(uri::decode("http://ABC12345zef") == "http://ABC12345zef");
    require(uri::decode("%20%RQabcABV") == " %RQabcABV");
    require(uri::decode("%c3%a9") == "\xC3\xA9");
    require(uri::decode("%\xC3\xA9%E9") == "%\xC3\xA9\xE9");  // Non-ASCII bytes after '%' are kept as they are
}};

Scenario uRIParsing{"URI parsing", [] {
    given("An URI with no userinfo nor fragment", [&] {
        uri::URI u0("http://localhost:80/foo.html?&q=1:2:3");
        when("decoded", [&] {
            require(u0.scheme == "http");
            require(u0.userinfo.empty());
            require(u0.host == "localhost");
            require(u0.port == "80");
            require(u0.path == "/foo.html");
            require(u0.authority == "localhost:80");
            require(u0.query == "&q=1:2:3");
            require(u0.fragment.empty());
        });
    });

    given("A complete URI", [&] {
        uri::URI u1("https://john.doe@www.example.com:123/forum/questions/?tag=networking&order=newest#top");
        when("decoded", [&] {
            require(u1.scheme == "https");
            require(u1.userinfo == "john.doe");
            require(u1.host == "www.example.com");
            require(u1.port == "123");
            require(u1.path == "/forum/questions/");
            require(u1.authority == "john.doe@www.example.com:123");
            require(u1.query == "tag=networking&order=newest");
            require(u1.fragment == "top");
        });
    });

    given("A Relative URI", [&] {
        uri::URI u2("/forum/questions/?tag=networking&order=newest#top");
        when("decoded", [&] {
            require(u2.scheme.empty());
            require(u2.userinfo.empty());
            require(u2.host.empty());
            require(u2.port.empty());
            require(u2.path == "/forum/questions/");
            require(u2.authority.empty());
            require(u2.query == "tag=networking&order=newest");
            require(u2.fragment == "top");
        });
    });

    given("A function which returns an URI from a string", [&] {
        auto parse = [](std::string text) { return uri::URI(text); };

        when("parsing URI without userinfo", [&] {
            auto u = parse("https://www.example.com:123/forum/questions/?tag=networking&order=newest#top");
            then("fields should be", [&] {
                require(u.scheme == "https");
                require(u.userinfo.empty());
                require(u.host == "www.example.com");
                require(u.port == "123");
                require(u.path == "/forum/questions/");
                require(u.authority == "www.example.com:123");
                require(u.query == "tag=networking&order=newest");
                require(u.fragment == "top");
            });
        });

        when("parsing URI without userinfo nor port", [&] {
            auto u = parse("https://www.example.com/forum/questions/?tag=networking&order=newest#top");
            then("fields should be", [&] {
                require(u.scheme == "https");
                require(u.userinfo.empty());
                require(u.host == "www.example.com");
                require(u.port.empty());
                require(u.path == "/forum/questions/");
                require(u.authority == "www.example.com");
                require(u.query == "tag=networking&order=newest");
                require(u.fragment == "top");
            });
        });

        when("parsing URI without userinfo nor port or query", [&] {
            auto u = parse("https://www.example.com/forum/questions/");
            then("fields should be", [&] {
                require(u.scheme == "https");
                require(u.userinfo.empty());
                require(u.host == "www.example.com");
                require(u.port.empty());
                require(u.path == "/forum/questions/");
                require(u.authority == "www.example.com");
                require(u.query.empty());
                require(u.fragment.empty());
            });
        });
    });
}};

Scenario webSocketSec{"WebSocket Sec Key coding", [] {
    std::string key = "x3JJHMbDL1EzLkh9GBhXDw==";
    require(base64::encodeInNetworkOrder(crypto::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) ==
            "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");

    key = "dGhlIHNhbXBsZSBub25jZQ==";
    require(base64::encodeInNetworkOrder(crypto::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}};

Scenario base64Encoding{"Base64 encoding", [] {
    require(base64::encodeInNetworkOrder(crypto::sha1("The quick brown fox jumps over the lazy dog")) ==
            "L9ThxnotKPzthJ7hu3bnORuT6xI=");
    require(base64::encodeInNetworkOrder(crypto::sha1("The quick brown fox jumps over the lazy cog")) ==
            "3p8sf9JeGzr60+haC9F9mxANtLM=");
    require(base64::encodeInNetworkOrder(crypto::sha1("")) == "2jmj7l5rSw0yVb/vlWAYkK/YBwk=");
}};

Scenario sHA1Hashing{"SHA1 hashing", [] {
    require(crypto::sha1String("The quick brown fox jumps over the lazy dog") == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    require(crypto::sha1String("The quick brown fox jumps over the lazy cog") == "de9f2c7fd25e1b3afad3e85a0bd17d9b100db4b3");
    require(crypto::sha1String("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}};