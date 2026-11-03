/// Dynamic routes of the HTTP server (#454): registration, priority, streamed replies, failures and shutdown.
#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/BlockingWorker.hpp>
#include <webfront/http/HTTPServer.hpp>

#include <atomic>
#include <fstream>
#include <chrono>
#include <list>
#include <string>
#include <string_view>
#include <future>
#include <thread>
#ifndef _WIN32
    #include <sys/resource.h>
#endif

using namespace webfront;
using namespace webfront::http;
using namespace std::chrono_literals;
using webfront::testing::InjectableSocket;

namespace {

/// One connection, accepted at once, whose request the test delivers once its routes are registered.
struct RouteNet : webfront::testing::InjectableNetworking {
    static inline std::atomic<int> accepts{0};
    static auto                    Accept(auto&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (accepts++ != 0) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

struct ProviderFiles {
    static inline std::list<std::string_view> names{"webfront/services/files/provided.txt"};
};
using Files  = MockFileSystem<ProviderFiles>;
using Router = Server<RouteNet, Files>;

template <typename Condition>
bool waitFor(Condition condition) {
    for (auto waited = 0ms; waited < 10s; waited += 1ms) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void send(std::string_view request) {
    std::vector<std::byte> bytes;
    for (const char c : request) bytes.push_back(static_cast<std::byte>(c));
    InjectableSocket::receive(bytes);
}

std::string get(std::string_view target, std::string_view method = "GET") {
    return std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

struct Fixture {
    Fixture() {
        InjectableSocket::reset();
        RouteNet::accepts = 0;
    }
    ~Fixture() { InjectableSocket::reset(); }

    /// Sends the request and waits for the end of the connection; returns what the server wrote.
    std::string answer(std::string_view request) {
        send(request);
        REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
        return InjectableSocket::writtenText();
    }
};

RouteReply text(std::string body) {
    RouteReply reply;
    reply.headers.emplace_back("Content-Type", "text/plain");
    reply.headers.emplace_back("Cache-Control", "no-store");
    reply.contentLength = body.size();
    reply.body          = [body = std::move(body), position = std::size_t{0}](std::span<char> out) mutable {
        const auto count = std::min(out.size(), body.size() - position);
        std::copy_n(body.data() + position, count, out.data());
        position += count;
        return count;
    };
    return reply;
}

bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

} // namespace

SCENARIO("Route prefixes are validated when registered") {
    RouteTable table;
    const auto handler = [](const RouteRequest&) { return RouteReply{}; };
    GIVEN("prefixes that cannot be routes") {
        for (const auto* prefix : {"", "/", "/files", "files/", "/a//b/", "/a/../b/", "/a b/", "/a?b/", "/a#/", "/a%2e/"}) {
            THEN("'" << prefix << "' is refused") { REQUIRE_THROWS_AS(table.add(prefix, handler), std::invalid_argument); }
        }
        THEN("a route without handler is refused") { REQUIRE_THROWS_AS(table.add("/webfront/services/files/", nullptr), std::invalid_argument); }
    }
    GIVEN("a registered prefix") {
        auto registration = table.add("/webfront/services/files/", handler);
        THEN("the same prefix, a longer one and a shorter one collide") {
            REQUIRE_THROWS_AS(table.add("/webfront/services/files/", handler), std::invalid_argument);
            REQUIRE_THROWS_AS(table.add("/webfront/services/files/deep/", handler), std::invalid_argument);
            REQUIRE_THROWS_AS(table.add("/webfront/services/", handler), std::invalid_argument);
        }
        THEN("a sibling prefix, even sharing the name's start, is accepted") {
            REQUIRE_NOTHROW(table.add("/webfront/services/files2/", handler));
            REQUIRE_NOTHROW(table.add("/webfront/services/dialogs/", handler));
        }
        THEN("lookup matches paths under the prefix only") {
            REQUIRE(table.find("/webfront/services/files/token"));
            REQUIRE_FALSE(table.find("/webfront/services/files"));
            REQUIRE_FALSE(table.find("/webfront/services/filesX/token"));
            REQUIRE_FALSE(table.find("/WebFront.js"));
        }
        THEN("unregistering frees the prefix and the lookup") {
            registration.reset();
            REQUIRE_FALSE(table.find("/webfront/services/files/token"));
            REQUIRE_NOTHROW(table.add("/webfront/services/files/", handler));
        }
        THEN("a moved registration keeps the route until it is destroyed") {
            auto moved = std::move(registration);
            REQUIRE(table.find("/webfront/services/files/x"));
            moved = {};
            REQUIRE_FALSE(table.find("/webfront/services/files/x"));
        }
    }
    GIVEN("a registration outliving its table") {
        RouteTable::Registration registration;
        {
            RouteTable shortLived;
            registration = shortLived.add("/webfront/services/files/", handler);
        }
        THEN("destroying it is harmless") { REQUIRE_NOTHROW(registration.reset()); }
    }
}

SCENARIO("A route streams its reply in bounded chunks") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    GIVEN("a body several chunks long") {
        std::string body;
        for (std::size_t index = 0; index < 3 * DefaultBuffersPolicy::emissionBufferSize + 5; ++index) body.push_back(static_cast<char>('a' + index % 26));
        std::size_t largestChunk = 0;
        std::string target, query;
        auto        route = server.addRoute("/webfront/services/files/", [&](const RouteRequest& request) {
            target = std::string(request.path);
            query  = std::string(request.query);
            auto reply = text(body);
            reply.body = [inner = std::move(reply.body), &largestChunk](std::span<char> out) {
                largestChunk = std::max(largestChunk, out.size());
                return inner(out);
            };
            return reply;
        });
        WHEN("the browser requests it with a percent-encoded name and a query") {
            const auto response = fixture.answer(get("/webfront/services/files/a%20b.txt?token=1"));
            THEN("the headers describe the reply and the whole body follows") {
                REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                REQUIRE(contains(response, "Content-Type: text/plain\r\n"));
                REQUIRE(contains(response, "Cache-Control: no-store\r\n"));
                REQUIRE(contains(response, "Content-Length: " + std::to_string(body.size()) + "\r\n"));
                REQUIRE(contains(response, "Connection: close\r\n"));
                REQUIRE(response.ends_with("\r\n\r\n" + body));
            }
            THEN("the handler saw the decoded path and the raw query") {
                REQUIRE(target == "/webfront/services/files/a b.txt");
                REQUIRE(query == "token=1");
            }
            THEN("the body was never asked for more than the emission buffer at once") {
                REQUIRE(largestChunk == DefaultBuffersPolicy::emissionBufferSize);
            }
        }
    }
}

SCENARIO("A route answers HEAD without reading its body") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    std::atomic<int> reads{0};
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        auto reply = text("0123456789");
        reply.body = [&reads](std::span<char>) { ++reads; return std::size_t{0}; };
        return reply;
    });
    const auto response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
    THEN("the headers announce the length and no body follows") {
        REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(contains(response, "Content-Length: 10\r\n"));
        REQUIRE(response.ends_with("\r\n\r\n"));
        REQUIRE(reads == 0);
    }
}

SCENARIO("A route precedes the file providers and refuses other methods") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    std::atomic<int> calls{0};
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        ++calls;
        return text("from the route");
    });
    GIVEN("a provider that also has a file under the prefix") {
        const auto response = fixture.answer(get("/webfront/services/files/provided.txt"));
        THEN("the route answers") { REQUIRE(response.ends_with("from the route")); }
    }
    GIVEN("a POST") {
        const auto response = fixture.answer(get("/webfront/services/files/x", "POST"));
        THEN("it is refused with the methods the route serves, without calling the handler") {
            REQUIRE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
            REQUIRE(contains(response, "Allow: GET, HEAD\r\n"));
            REQUIRE(calls == 0);
        }
    }
}

SCENARIO("A path outside every route, or after unregistering, goes to the providers") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    auto    route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) { return text("x"); });
    GIVEN("a registration that was reset") {
        route.reset();
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("the request is a 404") { REQUIRE(response.starts_with("HTTP/1.1 404 Not Found\r\n")); }
    }
}

SCENARIO("A failing handler answers 500 without leaking its error") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    GIVEN("a handler that throws") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) -> RouteReply { throw std::runtime_error("secret detail"); });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("the browser gets a bare 500") {
            REQUIRE(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
            REQUIRE_FALSE(contains(response, "secret detail"));
        }
    }
    GIVEN("a handler whose header would split the response") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            auto reply = text("x");
            reply.headers.emplace_back("X-Name", "a\r\nSet-Cookie: injected=1");
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("it is refused") {
            REQUIRE(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
            REQUIRE_FALSE(contains(response, "injected"));
        }
    }
    GIVEN("a handler setting a header the server owns") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            auto reply = text("x");
            reply.headers.emplace_back("content-length", "1");
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("it is refused") { REQUIRE(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n")); }
    }
}

SCENARIO("A body that breaks its promise closes the connection") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    GIVEN("a body shorter than its Content-Length") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            auto reply          = text("abc");
            reply.contentLength = 10;
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("the client receives a truncated body, which it detects") {
            REQUIRE(contains(response, "Content-Length: 10\r\n"));
            REQUIRE(response.ends_with("\r\n\r\nabc"));
        }
    }
    GIVEN("a body longer than its Content-Length") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            auto reply          = text("abcdef");
            reply.contentLength = 3;
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("nothing past the announced length is sent") { REQUIRE(response.ends_with("\r\n\r\n")); }
    }
    GIVEN("a body that throws after a first chunk") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            RouteReply reply;
            reply.contentLength = 100;
            reply.body          = [first = true](std::span<char> out) mutable -> std::size_t {
                if (!std::exchange(first, false)) throw std::runtime_error("disk gone");
                out[0] = 'x';
                return 1;
            };
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x"));
        THEN("the connection ends after what was read") { REQUIRE(response.ends_with("\r\n\r\nx")); }
    }
}

SCENARIO("A slow handler does not block the server context") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    std::atomic<bool> entered{false}, release{false};
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        entered = true;
        while (!release) std::this_thread::sleep_for(1ms);
        return text("late");
    });
    send(get("/webfront/services/files/x"));
    REQUIRE(waitFor([&] { return entered.load(); }));
    THEN("the thread which sent the request has already returned, and the reply comes when the handler ends") {
        REQUIRE(InjectableSocket::shutdownCount() == 0);
        release = true;
        REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
        REQUIRE(InjectableSocket::writtenText().ends_with("late"));
    }
}

namespace {
// Peak resident set of the process in bytes, or 0 where it cannot be read.
std::uint64_t peakResidentBytes() {
#ifdef _WIN32
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    #ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);  // bytes
    #else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;  // kilobytes
    #endif
#endif
}
}  // namespace

SCENARIO("A large body is streamed without being held") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    constexpr std::uint64_t size = 300ull * 1024 * 1024;
    std::size_t             largestChunk = 0;
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        RouteReply reply;
        reply.contentLength = size;
        reply.body          = [&largestChunk, produced = std::uint64_t{0}](std::span<char> out) mutable {
            largestChunk     = std::max(largestChunk, out.size());
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size - produced));
            produced += count;
            return count;
        };
        return reply;
    });
    InjectableSocket::discardWrites();
    const auto before = peakResidentBytes();
    send(get("/webfront/services/files/big"));
    REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
    THEN("the peak memory of the process did not grow with the size sent") {
        // Holding the body would add its 300 MiB to the peak; ctest runs each scenario in its own process.
        if (const auto after = peakResidentBytes()) REQUIRE(after - before < 64ull * 1024 * 1024);
    }
    THEN("every byte was sent through a buffer of the emission size") {
        REQUIRE(InjectableSocket::discardedBytes() > size);
        REQUIRE(largestChunk == DefaultBuffersPolicy::emissionBufferSize);
    }
}

SCENARIO("A connection which fails while a reply is streamed stops the reading") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    std::atomic<int> reads{0};
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        RouteReply reply;
        reply.body = [&reads](std::span<char> out) {
            if (++reads == 2) InjectableSocket::failWrites(std::make_error_code(std::errc::connection_reset));
            return out.size();
        };
        return reply;
    });
    send(get("/webfront/services/files/endless"));
    REQUIRE(waitFor([] { return InjectableSocket::shutdownCount() > 0; }));
    THEN("the body is not read again") {
        const auto seen = reads.load();
        std::this_thread::sleep_for(20ms);
        REQUIRE(reads == seen);
        REQUIRE(seen <= 3);
    }
}

SCENARIO("Stopping the server ends a reply being streamed") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    auto route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
        RouteReply reply;
        reply.body = [](std::span<char> out) { return out.size(); };
        return reply;
    });
    InjectableSocket::deferWrites();
    send(get("/webfront/services/files/endless"));
    REQUIRE(waitFor([] { return InjectableSocket::hasWrittenData(); }));
    WHEN("the server is stopped and drained while a send is pending") {
        server.stop();
        THEN("the drain returns") { REQUIRE_NOTHROW(server.drain()); }
    }
}

namespace {

std::string pattern(std::size_t size) {
    std::string bytes;
    for (std::size_t index = 0; index < size; ++index) bytes.push_back(static_cast<char>((index * 7 + 3) % 251));
    return bytes;
}

// Serves, whatever the path, a file of fixed content: embedded data, or a native file when `native` names one.
struct ProvidedFile {
    static inline std::string                content;
    static inline std::vector<std::uint64_t> words;
    static inline std::filesystem::path      native;
};
struct ProvidedFS {
    explicit ProvidedFS(std::filesystem::path) {}
    std::optional<fs::File> open(std::filesystem::path) {
        if (!ProvidedFile::native.empty()) return fs::File(std::ifstream(ProvidedFile::native, std::ios::binary));
        auto& words = ProvidedFile::words;
        words.assign((ProvidedFile::content.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < ProvidedFile::content.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(ProvidedFile::content[index])) << ((7 - index % 8) * 8);
        return fs::File(words, ProvidedFile::content.size());
    }
};

} // namespace

SCENARIO("Files of a provider are announced by size and streamed") {
    for (const bool native : {false, true}) {
        GIVEN(std::string(native ? "a native file" : "an embedded file") << " of several chunks") {
            webfront::test::TemporaryDirectory directory;
            ProvidedFile::content = pattern(3 * DefaultBuffersPolicy::emissionBufferSize + 11);
            ProvidedFile::native.clear();
            if (native) {
                ProvidedFile::native = directory.path() / "file.bin";
                std::ofstream(ProvidedFile::native, std::ios::binary) << ProvidedFile::content;
            }
            Fixture                     fixture;
            Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
            WHEN("it is requested") {
                const auto response = fixture.answer(get("/file.bin"));
                THEN("the length is announced and the whole content follows") {
                    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    REQUIRE(contains(response, "Content-Length: " + std::to_string(ProvidedFile::content.size()) + "\r\n"));
                    REQUIRE(response.ends_with("\r\n\r\n" + ProvidedFile::content));
                }
            }
        }
        GIVEN(std::string(native ? "a native file" : "an embedded file") << " requested with HEAD") {
            webfront::test::TemporaryDirectory directory;
            ProvidedFile::content = pattern(20000);
            ProvidedFile::native.clear();
            if (native) {
                ProvidedFile::native = directory.path() / "file.bin";
                std::ofstream(ProvidedFile::native, std::ios::binary) << ProvidedFile::content;
            }
            Fixture                     fixture;
            Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
            const auto response = fixture.answer(get("/file.bin", "HEAD"));
            THEN("the length is announced and no body follows") {
                REQUIRE(contains(response, "Content-Length: 20000\r\n"));
                REQUIRE(response.ends_with("\r\n\r\n"));
            }
        }
    }
    GIVEN("an empty file") {
        ProvidedFile::content = "";
        ProvidedFile::native.clear();
        Fixture                     fixture;
        Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
        const auto response = fixture.answer(get("/file.bin"));
        THEN("it is a 200 with a zero length") {
            REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(contains(response, "Content-Length: 0\r\n"));
        }
    }
}

SCENARIO("A native file known by its size reports it") {
    webfront::test::TemporaryDirectory directory;
    const auto                         path = directory.path() / "known.bin";
    std::ofstream(path, std::ios::binary) << pattern(1234);
    THEN("knownSize is the size on disk, whatever was read") {
        fs::File file{std::ifstream(path, std::ios::binary)};
        REQUIRE(file.knownSize() == 1234);
        std::array<char, 100> buffer{};
        REQUIRE(file.read(buffer) == 100);
        REQUIRE(file.knownSize() == 1234);
    }
}

SCENARIO("Failed HEAD requests carry no body") {
    Fixture fixture;
    Router  server("127.0.0.1", "0");
    GIVEN("a handler which throws") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) -> RouteReply { throw std::runtime_error("no"); });
        const auto response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
        THEN("the 500 has its headers only") {
            REQUIRE(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
            REQUIRE(response.ends_with("\r\n\r\n"));
        }
    }
    GIVEN("a handler with an invalid header") {
        auto route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
            auto reply = text("x");
            reply.headers.emplace_back("Bad Name", "v");
            return reply;
        });
        const auto response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
        THEN("the 500 has its headers only") { REQUIRE(response.ends_with("\r\n\r\n")); }
    }
    GIVEN("a file which does not exist") {
        const auto response = fixture.answer(get("/nothing.txt", "HEAD"));
        THEN("the 404 has its headers only") {
            REQUIRE(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
            REQUIRE(response.ends_with("\r\n\r\n"));
        }
    }
}

SCENARIO("A file wrapped after a prefix yields the remaining bytes and their count") {
    webfront::test::TemporaryDirectory directory;
    const auto                         path = directory.path() / "prefixed.bin";
    std::ofstream(path, std::ios::binary) << "prefix:payload";
    std::ifstream stream(path, std::ios::binary);
    stream.seekg(7);
    fs::File file{std::move(stream)};
    THEN("only the payload is counted and read") {
        REQUIRE(file.knownSize() == 7);
        std::array<char, 32> buffer{};
        const auto           count = file.read(buffer);
        REQUIRE(std::string(buffer.data(), count) == "payload");
    }
}

namespace {
struct Completes {
    using receiver_concept = networking::ex::receiver_tag;
    std::promise<void>* done;
    void                set_value() noexcept { done->set_value(); }
    void                set_error(std::exception_ptr) noexcept { done->set_value(); }
    void                set_stopped() noexcept { done->set_value(); }
};
}  // namespace

SCENARIO("A blocked job does not delay the others of a worker with several threads") {
    BlockingWorker     worker{2};
    std::promise<void> release, blockedDone, quickDone;
    auto               shared = release.get_future().share();
    std::atomic<bool>  blocked{false};
    auto               blockedEnded = blockedDone.get_future();
    auto               quickEnded   = quickDone.get_future();
    auto               slow = networking::ex::connect(worker.run([&] { blocked = true; shared.wait(); }), Completes{&blockedDone});
    auto               fast = networking::ex::connect(worker.run([] {}), Completes{&quickDone});
    networking::ex::start(slow);
    REQUIRE(waitFor([&] { return blocked.load(); }));
    networking::ex::start(fast);
    THEN("the quick job completes while the slow one still runs") {
        REQUIRE(quickEnded.wait_for(5s) == std::future_status::ready);
        REQUIRE(blockedEnded.wait_for(10ms) == std::future_status::timeout);
    }
    release.set_value();
    blockedEnded.wait();
}

SCENARIO("A single-thread worker keeps the jobs in order") {
    BlockingWorker     worker;
    std::promise<void> firstDone, secondDone;
    std::string        order;
    auto               first  = networking::ex::connect(worker.run([&] { order += 'a'; }), Completes{&firstDone});
    auto               second = networking::ex::connect(worker.run([&] { order += 'b'; }), Completes{&secondDone});
    networking::ex::start(first);
    networking::ex::start(second);
    firstDone.get_future().wait();
    secondDone.get_future().wait();
    REQUIRE(order == "ab");
}
