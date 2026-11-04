/// Byte range requests (#455): the selection of a range, and its use by files and by dynamic routes.
#include "GzipFixtures.hpp"
#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/http/Ranges.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace webfront;
using namespace webfront::http;
using namespace std::chrono_literals;
using webfront::testing::InjectableSocket;

namespace {

/// The header fields of a request, as `range::select` reads them.
struct Fields {
    std::vector<std::pair<std::string, std::string>> fields;
    std::list<std::string>                           getHeadersValues(std::string_view name) const {
        std::list<std::string> values;
        for (const auto& [key, value] : fields)
            if (key == name) values.push_back(value);
        return values;
    }
};

Fields withRange(std::string value) { return {{{"Range", std::move(value)}}}; }

using range::Outcome;

struct Expected {
    Outcome       outcome;
    std::uint64_t first  = 0;
    std::uint64_t length = 0;
};

void expectSelection(const Fields& request, std::uint64_t size, Expected expected, const std::optional<std::string>& tag = {}) {
    const auto selection = range::select(request, size, tag);
    REQUIRE(selection.outcome == expected.outcome);
    if (expected.outcome == Outcome::partial) {
        REQUIRE(selection.first == expected.first);
        REQUIRE(selection.length == expected.length);
    }
}

struct RangeNet : webfront::testing::InjectableNetworking {
    static inline std::atomic<int> accepts{0};
    static auto                    Accept(auto&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (accepts++ != 0) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

bool waitFor(auto condition) {
    for (auto waited = 0ms; waited < 10s; waited += 1ms) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

/// What the provider below serves, whatever the path asked.
struct Served {
    static inline std::string                content;
    static inline std::vector<std::uint64_t> words;
    static inline std::filesystem::path      native;  ///< a native file when set, embedded data otherwise
    static inline std::string_view           encoding;
    static inline std::string                tag;       ///< the provider's validator; empty when it has none
};

struct ServedFS {
    explicit ServedFS(std::filesystem::path) {}
    std::optional<fs::File> open(std::filesystem::path) {
        if (!Served::native.empty()) {
            fs::File file{std::ifstream(Served::native, std::ios::binary)};
            if (!Served::tag.empty()) file.setEntityTag(Served::tag);
            return file;
        }
        auto& words = Served::words;
        words.assign((Served::content.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < Served::content.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(Served::content[index])) << ((7 - index % 8) * 8);
        return fs::File(words, Served::content.size(), Served::encoding);
    }
};

std::string pattern(std::size_t size) {
    std::string text;
    for (std::size_t index = 0; index < size; ++index) text.push_back(static_cast<char>('a' + (index * 7 + index / 13) % 26));
    return text;
}

std::string request(std::string_view method, std::string_view headers) {
    return std::string(method) + " /media.bin HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n" + std::string(headers) + "\r\n";
}

/// Serves one request on a new server and returns everything it wrote.
std::string fetch(const std::string& text) {
    InjectableSocket::reset();
    RangeNet::accepts = 0;
    http::Server<RangeNet, ServedFS> server("127.0.0.1", "0");
    std::vector<std::byte>           bytes;
    for (const char c : text) bytes.push_back(static_cast<std::byte>(c));
    InjectableSocket::receive(bytes);
    REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
    auto written = InjectableSocket::writtenText();
    InjectableSocket::reset();
    return written;
}

bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

std::string body(const std::string& response) {
    const auto end = response.find("\r\n\r\n");
    return end == std::string::npos ? std::string{} : response.substr(end + 4);
}

std::string header(const std::string& response, std::string_view name) {
    const auto head  = response.substr(0, response.find("\r\n\r\n"));
    const auto start = head.find("\r\n" + std::string(name) + ": ");
    if (start == std::string::npos) return {};
    const auto from = start + 4 + name.size();
    return head.substr(from, head.find("\r\n", from) - from);
}

void serveEmbedded(std::string content, std::string_view encoding = {}) {
    Served::content  = std::move(content);
    Served::encoding = encoding;
    Served::tag.clear();
    Served::native.clear();
}

} // namespace

SCENARIO("A Range header selects one part of a representation") {
    constexpr std::uint64_t size = 1000;
    GIVEN("well-formed single ranges") {
        THEN("first-last selects the bytes, last being clamped to the end") {
            expectSelection(withRange("bytes=0-0"), size, {Outcome::partial, 0, 1});
            expectSelection(withRange("bytes=10-19"), size, {Outcome::partial, 10, 10});
            expectSelection(withRange("bytes=990-5000"), size, {Outcome::partial, 990, 10});
            expectSelection(withRange("bytes=0-999"), size, {Outcome::partial, 0, 1000});
            expectSelection(withRange("bytes=999-999"), size, {Outcome::partial, 999, 1});
        }
        THEN("first- selects up to the end") {
            expectSelection(withRange("bytes=0-"), size, {Outcome::partial, 0, 1000});
            expectSelection(withRange("bytes=900-"), size, {Outcome::partial, 900, 100});
            expectSelection(withRange("bytes=999-"), size, {Outcome::partial, 999, 1});
        }
        THEN("-n selects the last n bytes, the whole when n exceeds the size") {
            expectSelection(withRange("bytes=-1"), size, {Outcome::partial, 999, 1});
            expectSelection(withRange("bytes=-100"), size, {Outcome::partial, 900, 100});
            expectSelection(withRange("bytes=-1000"), size, {Outcome::partial, 0, 1000});
            expectSelection(withRange("bytes=-5000"), size, {Outcome::partial, 0, 1000});
        }
        THEN("the unit is case-insensitive and surrounding blanks are ignored") {
            expectSelection(withRange("Bytes=5-6"), size, {Outcome::partial, 5, 2});
            expectSelection(withRange("  bytes=5-6 \t"), size, {Outcome::partial, 5, 2});
        }
    }
    GIVEN("ranges nothing can satisfy") {
        THEN("a first byte at or past the end is unsatisfiable") {
            expectSelection(withRange("bytes=1000-"), size, {Outcome::unsatisfiable});
            expectSelection(withRange("bytes=1000-1001"), size, {Outcome::unsatisfiable});
            expectSelection(withRange("bytes=99999999999999999999999-"), size, {Outcome::unsatisfiable});
        }
        THEN("an empty suffix is unsatisfiable") { expectSelection(withRange("bytes=-0"), size, {Outcome::unsatisfiable}); }
        THEN("any range of an empty representation is unsatisfiable") {
            expectSelection(withRange("bytes=0-0"), 0, {Outcome::unsatisfiable});
            expectSelection(withRange("bytes=0-"), 0, {Outcome::unsatisfiable});
            expectSelection(withRange("bytes=-5"), 0, {Outcome::unsatisfiable});
        }
    }
    GIVEN("headers the server ignores") {
        THEN("the whole representation is answered") {
            expectSelection({}, size, {Outcome::whole});
            expectSelection(withRange("items=0-5"), size, {Outcome::whole});
            expectSelection(withRange("bytes"), size, {Outcome::whole});
            expectSelection(withRange("bytes="), size, {Outcome::whole});
            expectSelection(withRange("bytes=-"), size, {Outcome::whole});
            expectSelection(withRange("bytes=5-2"), size, {Outcome::whole});
            expectSelection(withRange("bytes=a-b"), size, {Outcome::whole});
            expectSelection(withRange("bytes=+1-2"), size, {Outcome::whole});
            expectSelection(withRange("bytes=1 -2"), size, {Outcome::whole});
            expectSelection(withRange("bytes=0-1,5-6"), size, {Outcome::whole});
            expectSelection(withRange("bytes=0-1,"), size, {Outcome::whole});
            expectSelection({{{"Range", "bytes=0-1"}, {"Range", "bytes=5-6"}}}, size, {Outcome::whole});
        }
    }
    GIVEN("a number beyond 64 bits") {
        THEN("it saturates instead of wrapping") {
            expectSelection(withRange("bytes=0-99999999999999999999999"), size, {Outcome::partial, 0, 1000});
            expectSelection(withRange("bytes=-99999999999999999999999"), size, {Outcome::partial, 0, 1000});
        }
    }
    GIVEN("an If-Range condition") {
        const std::optional<std::string> tag{"\"1000-42\""};
        auto                             conditioned = [](std::string condition) {
            return Fields{{{"Range", "bytes=10-19"}, {"If-Range", std::move(condition)}}};
        };
        THEN("the range applies when it is the strong tag of the representation") {
            expectSelection(conditioned("\"1000-42\""), size, {Outcome::partial, 10, 10}, tag);
        }
        THEN("the whole representation is answered when the tag differs, is weak, is a date or cannot be compared") {
            expectSelection(conditioned("\"1000-43\""), size, {Outcome::whole}, tag);
            expectSelection(conditioned("W/\"1000-42\""), size, {Outcome::whole}, tag);
            expectSelection(conditioned("Sun, 06 Nov 1994 08:49:37 GMT"), size, {Outcome::whole}, tag);
            expectSelection(conditioned("\"1000-42\""), size, {Outcome::whole});
            expectSelection({{{"Range", "bytes=10-19"}, {"If-Range", "\"1000-42\""}, {"If-Range", "\"1000-42\""}}}, size, {Outcome::whole}, tag);
        }
        THEN("an If-Range without Range does nothing") {
            expectSelection({{{"If-Range", "\"1000-42\""}}}, size, {Outcome::whole}, tag);
        }
    }
    THEN("the Content-Range headers spell the bounds and the size") {
        REQUIRE(range::contentRange({Outcome::partial, 10, 10}, 1000) == "bytes 10-19/1000");
        REQUIRE(range::unsatisfiedContentRange(1000) == "bytes */1000");
    }
}

SCENARIO("A file is served by ranges") {
    const auto chunk = DefaultBuffersPolicy::emissionBufferSize;
    for (const bool native : {false, true}) {
        GIVEN(std::string(native ? "a native file" : "an embedded file") << " of several chunks") {
            webfront::test::TemporaryDirectory directory;
            const auto                         content = pattern(3 * chunk + 11);
            serveEmbedded(content);
            if (native) {
                Served::native = directory.path() / "media.bin";
                std::ofstream(Served::native, std::ios::binary) << content;
            }
            WHEN("the whole file is requested") {
                const auto response = fetch(request("GET", ""));
                THEN("200 announces the ranges") {
                    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(header(response, "Accept-Ranges") == "bytes");
                    REQUIRE(body(response) == content);
                }
                THEN("no validator is derived from a pathname, a size or a time") {
                    REQUIRE(header(response, "ETag").empty());
                }
            }
            WHEN("a range crosses the emission chunks") {
                const auto first    = chunk - 5;
                const auto response = fetch(request("GET", "Range: bytes=" + std::to_string(first) + "-" + std::to_string(2 * chunk + 4) + "\r\n"));
                THEN("206 sends exactly those bytes") {
                    REQUIRE(response.starts_with("HTTP/1.1 206 Partial Content\r\n"));
                    REQUIRE(header(response, "Content-Range") == "bytes " + std::to_string(first) + "-" + std::to_string(2 * chunk + 4) + "/" + std::to_string(content.size()));
                    REQUIRE(header(response, "Content-Length") == std::to_string(chunk + 10));
                    REQUIRE(body(response) == content.substr(first, chunk + 10));
                }
            }
            WHEN("the end of the file is requested") {
                THEN("an open range reads to the end") {
                    const auto response = fetch(request("GET", "Range: bytes=" + std::to_string(content.size() - 7) + "-\r\n"));
                    REQUIRE(response.starts_with("HTTP/1.1 206 Partial Content\r\n"));
                    REQUIRE(body(response) == content.substr(content.size() - 7));
                }
                THEN("a suffix reads the last bytes") {
                    const auto response = fetch(request("GET", "Range: bytes=-7\r\n"));
                    REQUIRE(body(response) == content.substr(content.size() - 7));
                    REQUIRE(header(response, "Content-Range") == "bytes " + std::to_string(content.size() - 7) + "-" + std::to_string(content.size() - 1) + "/" + std::to_string(content.size()));
                }
                THEN("a last byte past the end is clamped") {
                    const auto response = fetch(request("GET", "Range: bytes=" + std::to_string(content.size() - 3) + "-99999999\r\n"));
                    REQUIRE(body(response) == content.substr(content.size() - 3));
                }
            }
            WHEN("the range starts at the first byte") {
                const auto response = fetch(request("GET", "Range: bytes=0-0\r\n"));
                THEN("206 sends it without seeking") { REQUIRE(body(response) == content.substr(0, 1)); }
            }
            WHEN("the range starts past the end") {
                const auto response = fetch(request("GET", "Range: bytes=" + std::to_string(content.size()) + "-\r\n"));
                THEN("416 tells the size and sends no content") {
                    REQUIRE(response.starts_with("HTTP/1.1 416 Range Not Satisfiable\r\n"));
                    REQUIRE(header(response, "Content-Range") == "bytes */" + std::to_string(content.size()));
                    REQUIRE_FALSE(contains(response, content.substr(0, 40)));
                }
            }
            WHEN("several ranges are requested") {
                const auto response = fetch(request("GET", "Range: bytes=0-1,5-6\r\n"));
                THEN("the whole file is answered") {
                    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(body(response) == content);
                }
            }
            WHEN("HEAD carries a range") {
                const auto response = fetch(request("HEAD", "Range: bytes=10-29\r\n"));
                THEN("the range is ignored: it describes the whole file and sends no body") {
                    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(header(response, "Content-Length") == std::to_string(content.size()));
                    REQUIRE(header(response, "Content-Range").empty());
                    REQUIRE(header(response, "Accept-Ranges") == "bytes");
                    REQUIRE(body(response).empty());
                }
                THEN("an unsatisfiable range is ignored too") {
                    const auto past = fetch(request("HEAD", "Range: bytes=" + std::to_string(content.size()) + "-\r\n"));
                    REQUIRE(past.starts_with("HTTP/1.1 200 OK\r\n"));
                }
            }
        }
    }
}

SCENARIO("A range survives a file changed between two requests only when its validator still matches") {
    webfront::test::TemporaryDirectory directory;
    Served::native   = directory.path() / "media.bin";
    Served::encoding = {};
    // The provider versions its storage: here the tag is a hash of the content, which no time or size can replace.
    const auto publish = [](const std::string& content) {
        std::ofstream(Served::native, std::ios::binary | std::ios::trunc) << content;
        Served::tag = "\"v" + std::to_string(std::hash<std::string>{}(content)) + "\"";
    };
    const auto original = pattern(5000);
    publish(original);
    const auto tag = header(fetch(request("GET", "")), "ETag");
    REQUIRE(tag == Served::tag);
    const std::string conditional = "Range: bytes=100-199\r\nIf-Range: " + tag + "\r\n";

    GIVEN("the file did not change") {
        const auto response = fetch(request("GET", conditional));
        THEN("the range is served") {
            REQUIRE(response.starts_with("HTTP/1.1 206 Partial Content\r\n"));
            REQUIRE(body(response) == original.substr(100, 100));
        }
    }
    GIVEN("the file grew") {
        const auto replaced = pattern(5001);
        publish(replaced);
        const auto response = fetch(request("GET", conditional));
        THEN("the current file is sent in full, with its new tag") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(body(response) == replaced);
            REQUIRE(header(response, "ETag") != tag);
        }
    }
    GIVEN("the file was rewritten with the same size and the same modification time") {
        const auto before = std::filesystem::last_write_time(Served::native);
        std::string replaced = original;
        std::reverse(replaced.begin(), replaced.end());
        publish(replaced);
        std::filesystem::last_write_time(Served::native, before);
        const auto response = fetch(request("GET", conditional));
        THEN("the current file is sent in full") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(body(response) == replaced);
        }
    }
    GIVEN("a native file whose provider gives no validator") {
        Served::tag.clear();
        const auto response = fetch(request("GET", "Range: bytes=100-199\r\nIf-Range: " + tag + "\r\n"));
        THEN("it has no ETag and a conditioned range is answered in full") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(header(response, "ETag").empty());
            REQUIRE(body(response) == original);
        }
        THEN("a range without condition is still served") {
            const auto plain = fetch(request("GET", "Range: bytes=100-199\r\n"));
            REQUIRE(plain.starts_with("HTTP/1.1 206 Partial Content\r\n"));
        }
    }
    GIVEN("the file shrank without the validator being checked") {
        std::ofstream(Served::native, std::ios::binary | std::ios::trunc) << pattern(150);
        const auto response = fetch(request("GET", "Range: bytes=100-199\r\n"));
        THEN("the range is clamped to the current file") {
            REQUIRE(response.starts_with("HTTP/1.1 206 Partial Content\r\n"));
            REQUIRE(body(response) == pattern(150).substr(100));
        }
    }
    GIVEN("embedded data, which has no validator") {
        serveEmbedded(original);
        const auto response = fetch(request("GET", "Range: bytes=100-199\r\nIf-Range: \"anything\"\r\n"));
        THEN("a conditioned range is answered in full") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(body(response) == original);
        }
    }
}

SCENARIO("A stored or decoded representation is not served by ranges") {
    const auto content = pattern(300);
    GIVEN("a gzip file the client accepts") {
        serveEmbedded(content, "gzip");
        const auto response = fetch(request("GET", "Range: bytes=10-19\r\n"));
        THEN("it is sent in full, as stored, and says it has no ranges") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(header(response, "Accept-Ranges") == "none");
            REQUIRE(header(response, "Content-Encoding") == "gzip");
        }
    }
    GIVEN("a gzip file the client does not accept, which the server decodes") {
        serveEmbedded(std::string(gzipfixtures::fixed), "gzip");
        const auto response = fetch("GET /media.bin HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: identity\r\nRange: bytes=10-19\r\n\r\n");
        THEN("the answer is never a 206 and says it has no ranges") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(body(response).starts_with("WebFront gzip fallback"));
            REQUIRE(header(response, "Accept-Ranges") == "none");
        }
    }
}

SCENARIO("A dynamic route serves a file by ranges") {
    for (const bool native : {false, true}) {
        GIVEN(std::string(native ? "a native file" : "an embedded file") << " behind a route") {
            webfront::test::TemporaryDirectory directory;
            const auto                         content = pattern(2 * DefaultBuffersPolicy::emissionBufferSize + 3);
            serveEmbedded(content);
            Served::native.clear();
            const auto path = directory.path() / "granted.bin";
            if (native) std::ofstream(path, std::ios::binary) << content;
            const auto open = [&] {
                if (native) return fs::File(std::ifstream(path, std::ios::binary));
                static std::vector<std::uint64_t> words;
                words.assign((content.size() + 7) / 8, 0);
                for (std::size_t index = 0; index < content.size(); ++index)
                    words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(content[index])) << ((7 - index % 8) * 8);
                return fs::File(words, content.size());
            };
            const auto answer = [&](const std::string& text) {
                InjectableSocket::reset();
                RangeNet::accepts = 0;
                http::Server<RangeNet, ServedFS> server("127.0.0.1", "0");
                auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest& routed) {
                    auto reply = fileReply(routed.request, open(), "video/mp4");
                    reply.headers.emplace_back("Cache-Control", "no-store");
                    return reply;
                });
                std::vector<std::byte> bytes;
                for (const char c : text) bytes.push_back(static_cast<std::byte>(c));
                InjectableSocket::receive(bytes);
                REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
                auto written = InjectableSocket::writtenText();
                InjectableSocket::reset();
                return written;
            };
            const auto get = [](std::string_view method, std::string_view headers) {
                return std::string(method) + " /webfront/services/files/token HTTP/1.1\r\nHost: localhost\r\n" + std::string(headers) + "\r\n";
            };
            WHEN("the file is requested in full, then twice by range on the same URL") {
                const auto whole   = answer(get("GET", ""));
                const auto middle  = answer(get("GET", "Range: bytes=1000-1999\r\n"));
                const auto resumed = answer(get("GET", "Range: bytes=2000-\r\n"));
                THEN("every request succeeds with its own bytes") {
                    REQUIRE(whole.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(header(whole, "Accept-Ranges") == "bytes");
                    REQUIRE(header(whole, "Content-Type") == "video/mp4");
                    REQUIRE(header(whole, "Cache-Control") == "no-store");
                    REQUIRE(body(whole) == content);
                    REQUIRE(middle.starts_with("HTTP/1.1 206 Partial Content\r\n"));
                    REQUIRE(header(middle, "Content-Range") == "bytes 1000-1999/" + std::to_string(content.size()));
                    REQUIRE(body(middle) == content.substr(1000, 1000));
                    REQUIRE(resumed.starts_with("HTTP/1.1 206 Partial Content\r\n"));
                    REQUIRE(body(resumed) == content.substr(2000));
                }
            }
            WHEN("the range is unsatisfiable") {
                const auto response = answer(get("GET", "Range: bytes=" + std::to_string(content.size()) + "-\r\n"));
                THEN("416 tells the size") {
                    REQUIRE(response.starts_with("HTTP/1.1 416 Range Not Satisfiable\r\n"));
                    REQUIRE(header(response, "Content-Range") == "bytes */" + std::to_string(content.size()));
                    REQUIRE(header(response, "Content-Length") == "0");
                }
            }
            WHEN("HEAD carries a range") {
                const auto response = answer(get("HEAD", "Range: bytes=5-14\r\n"));
                THEN("the range is ignored: it describes the whole file and sends no body") {
                    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(header(response, "Content-Length") == std::to_string(content.size()));
                    REQUIRE(body(response).empty());
                }
            }
        }
    }
}

SCENARIO("A route serving a stored encoding keeps it and negotiates it") {
    const auto content = pattern(64);
    const auto serve   = [&](std::string_view acceptEncoding) {
        InjectableSocket::reset();
        RangeNet::accepts = 0;
        http::Server<RangeNet, ServedFS> server("127.0.0.1", "0");
        std::vector<std::uint64_t> words((content.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < content.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(content[index])) << ((7 - index % 8) * 8);
        auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest& routed) {
            return fileReply(routed.request, fs::File(words, content.size(), "gzip"), "text/html");
        });
        const std::string text = "GET /webfront/services/files/page.html HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: " + std::string(acceptEncoding) +
                                 "\r\nRange: bytes=0-3\r\n\r\n";
        std::vector<std::byte> bytes;
        for (const char c : text) bytes.push_back(static_cast<std::byte>(c));
        InjectableSocket::receive(bytes);
        REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
        auto written = InjectableSocket::writtenText();
        InjectableSocket::reset();
        return written;
    };
    GIVEN("a client that accepts gzip") {
        const auto response = serve("gzip");
        THEN("the stored bytes are labelled with their coding, in full, whatever the Range") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(header(response, "Content-Encoding") == "gzip");
            REQUIRE(header(response, "Vary") == "Accept-Encoding");
            REQUIRE(header(response, "Accept-Ranges") == "none");
            REQUIRE(body(response) == content);
        }
    }
    GIVEN("a client that does not accept gzip") {
        const auto response = serve("identity");
        THEN("it gets a 406 instead of bytes it cannot read") {
            REQUIRE(response.starts_with("HTTP/1.1 406 Not Acceptable\r\n"));
            REQUIRE(header(response, "Vary") == "Accept-Encoding");
            REQUIRE(header(response, "Content-Encoding").empty());
        }
    }
}

SCENARIO("A native file can be read again from any position after its end") {
    webfront::test::TemporaryDirectory directory;
    const auto                         path = directory.path() / "short.bin";
    std::ofstream(path, std::ios::binary) << "abc";
    fs::File             file{std::ifstream(path, std::ios::binary)};
    std::array<char, 16> buffer{};
    REQUIRE(file.read(buffer) == 3);
    THEN("seek positions it again, and the content reads as before") {
        REQUIRE(file.seek(0));
        REQUIRE(file.read(buffer) == 3);
        REQUIRE(std::string(buffer.data(), 3) == "abc");
        REQUIRE(file.seek(2));
        REQUIRE(file.read(buffer) == 1);
        REQUIRE(buffer[0] == 'c');
    }
}
