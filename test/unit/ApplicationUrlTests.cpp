#include "Bdd.hpp"
#include <webfront/frontend/Url.hpp>

using namespace webfront::bdd;

const Scenario frontendURLs{"Frontend URLs use the configured listening address", [] {
    using webfront::frontend::detail::applicationUrl;
    then("IPv4 and IPv6 addresses are preserved and wildcard binds open loopback", [&] {
        require(applicationUrl("127.0.0.2", "9010", "index.html") == "http://127.0.0.2:9010/index.html");
        require(applicationUrl("::1", "9010", "index.html") == "http://[::1]:9010/index.html");
        require(applicationUrl("[::1]", "9010", "index.html") == "http://[::1]:9010/index.html");
        require(applicationUrl("0.0.0.0", "9010", "index.html") == "http://127.0.0.1:9010/index.html");
        require(applicationUrl("::", "9010", "index.html") == "http://[::1]:9010/index.html");
        require(applicationUrl("localhost", "9010", "index.html") == "http://localhost:9010/index.html");
    });
    then("addresses and ports cannot inject URL authority or shell syntax", [&] {
        requireThrows<std::invalid_argument>([&] { applicationUrl("", "9010", "index.html"); });
        requireThrows<std::invalid_argument>([&] { applicationUrl("127.0.0.1/evil", "9010", "index.html"); });
        requireThrows<std::invalid_argument>([&] { applicationUrl("127.0.0.1", "", "index.html"); });
        requireThrows<std::invalid_argument>([&] { applicationUrl("127.0.0.1", "90'10", "index.html"); });
    });
}};

const Scenario frontendURLsPreserve{"Frontend URLs preserve query and fragment syntax consistently", [] {
    using webfront::frontend::detail::applicationUrl;
    then("spaces and UTF-8 bytes are encoded without escaping URI delimiters", [&] {
        require(applicationUrl("127.0.0.1", "9002", "folder/a b.html?x=1&name=Ren\xc3\xa9#result")
                == "http://127.0.0.1:9002/folder/a%20b.html?x=1&name=Ren%C3%A9#result");
    });
    then("literal delimiters use percent escapes which are not encoded twice", [&] {
        require(applicationUrl("127.0.0.1", "9002", "a%23b%3Fc%25.html") == "http://127.0.0.1:9002/a%23b%3Fc%25.html");
        require(applicationUrl("127.0.0.1", "9002", "bad%xy%.html") == "http://127.0.0.1:9002/bad%25xy%25.html");
        require(applicationUrl("127.0.0.1", "9002", "a'b.html") == "http://127.0.0.1:9002/a%27b.html");
    });
}};
