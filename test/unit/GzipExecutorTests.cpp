#include "GzipFixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace {
namespace ex   = webfront::networking::ex;
namespace gzip = webfront::http::gzip;
using Net      = webfront::networking::TCPStdnet;
using namespace webfront::http;

struct Gate {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false, released = false;
    std::thread::id         worker;
    void                    enter() {
        std::unique_lock lock(mutex);
        entered = true;
        worker  = std::this_thread::get_id();
        changed.notify_all();
        changed.wait(lock, [&] {
            return released;
        });
    }
    void wait() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return entered;
        });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct GatedDecoder {
    std::shared_ptr<Gate>             gate;
    bool                              fail = false;
    std::shared_ptr<std::atomic_bool> throwOnce{};
    bool                              operator()(std::string_view input, std::string& output, std::size_t limit) const {
        gate->enter();
        if (fail || (throwOnce && throwOnce->exchange(false)))
            throw std::runtime_error("decode worker failure");
        return gzip::decodeGzipInto(input, output, limit);
    }
};
using Executor = gzip::DecodeExecutor<GatedDecoder>;
struct Completion {
    std::thread::id                thread;
    std::mutex                     mutex;
    std::condition_variable        changed;
    bool                           done = false, stopped = false;
    std::exception_ptr             error;
    std::function<void()>          onCompletion;
    std::optional<Executor::Lease> result;
    void                           wait() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return done;
        });
    }
};
struct Receiver {
    using receiver_concept = ex::receiver_tag;
    Completion&            observed;
    ex::inplace_stop_token token;
    auto                   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, token};
    }
    void set_value(Executor::Lease lease) && noexcept {
        if (observed.onCompletion)
            observed.onCompletion();
        std::lock_guard lock(observed.mutex);
        observed.thread = std::this_thread::get_id();
        observed.result.emplace(std::move(lease));
        observed.done = true;
        observed.changed.notify_all();
    }
    void set_stopped() && noexcept {
        if (observed.onCompletion)
            observed.onCompletion();
        std::lock_guard lock(observed.mutex);
        observed.stopped = observed.done = true;
        observed.changed.notify_all();
    }
    void set_error(std::exception_ptr error) && noexcept {
        if (observed.onCompletion)
            observed.onCompletion();
        std::lock_guard lock(observed.mutex);
        observed.error = error;
        observed.done  = true;
        observed.changed.notify_all();
    }
};
struct Finally {
    std::function<void()> action;
    ~Finally() {
        action();
    }
};

struct NeutralFS {
    std::vector<std::uint64_t>        data, other, corrupt, plain;
    static std::vector<std::uint64_t> pack(std::string_view bytes) {
        std::vector<std::uint64_t> words((bytes.size() + 7) / 8);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            words[i / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << ((7 - i % 8) * 8);
        return words;
    }
    explicit NeutralFS(std::filesystem::path) : data(pack(gzipfixtures::fixed)), plain(pack("progress")) {
        std::string bytes(gzipfixtures::fixed);
        bytes[4]                 = 1;
        other                    = pack(bytes);
        bytes[bytes.size() - 8] ^= 1;
        corrupt                  = pack(bytes);
    }
    std::optional<webfront::fs::File> open(std::filesystem::path path) {
        if (path == "allocation-failure.js")
            throw std::bad_alloc{};
        if (path == "plain.txt")
            return webfront::fs::File(plain, 8);
        if (path == "other.js")
            return webfront::fs::File(other, gzipfixtures::fixed.size(), "gzip");
        if (path == "corrupt.js")
            return webfront::fs::File(corrupt, gzipfixtures::fixed.size(), "gzip");
        return webfront::fs::File(data, gzipfixtures::fixed.size(), path == "alias.js" ? "x-gzip" : "gzip");
    }
};
struct OneJob : DefaultBuffersPolicy {
    static constexpr std::size_t maxGzipDecodeJobs = 1;
};
std::string freePort() {
    Net::IoContext context;
    Net::Acceptor  acceptor(context);
    auto           endpoint = Net::Resolve("127.0.0.1", "0");
    acceptor.open(endpoint.protocol());
    acceptor.bind(endpoint);
    return std::to_string(acceptor.local_endpoint().port());
}
void send(Net::Socket& socket, const std::string& port, std::string_view path, std::string_view extra = {}, bool head = false) {
    socket.connect(Net::Resolve("127.0.0.1", port));
    const auto request = std::string(head ? "HEAD " : "GET ") + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(extra) + "\r\n";
    socket.send(Net::Buffer(request));
}
std::string receive(Net::Socket& socket, bool head = false) {
    std::string            response;
    std::array<char, 4096> bytes{};
    for (;;) {
        const auto size = socket.receive(Net::Buffer(bytes));
        if (!size)
            break;
        response.append(bytes.data(), size);
        const auto end = response.find("\r\n\r\n");
        if (end == std::string::npos)
            continue;
        if (head || response.starts_with("HTTP/1.1 101"))
            break;
        const auto length = response.find("Content-Length: ");
        if (length != std::string::npos && response.size() >= end + 4 + std::stoull(response.substr(length + 16)))
            break;
    }
    return response;
}
}  // namespace

SCENARIO("Gzip CPU work coalesces revisions and bounds jobs, bytes and response leases") {
    auto       gate = std::make_shared<Gate>();
    Executor   executor({1, 2, 200000, 1000}, {gate});
    Completion first, head, full;
    auto       a       = ex::connect(executor.submit(std::string(gzipfixtures::fixed), 23768), Receiver{first, {}});
    auto       b       = ex::connect(executor.submit(std::string(gzipfixtures::fixed), 23768, true), Receiver{head, {}});
    auto       changed = std::string(gzipfixtures::fixed);
    changed[4]         = 1;
    auto    c          = ex::connect(executor.submit(changed, 23768), Receiver{full, {}});
    Finally cleanup{[&] {
        gate->release();
        executor.stop();
        executor.join();
    }};
    REQUIRE_FALSE(executor.statistics().workerStarted);
    ex::start(a);
    gate->wait();
    ex::start(b);
    ex::start(c);
    full.wait();
    REQUIRE_FALSE(static_cast<bool>(*full.result));
    REQUIRE(c.encoded.empty());
    REQUIRE(c.encoded.capacity() < gzipfixtures::fixed.size());
    REQUIRE(executor.statistics().jobs == 1);
    REQUIRE(executor.statistics().waiters == 2);
    REQUIRE(executor.statistics().bytes <= 200000);
    REQUIRE(executor.statistics().decodes == 1);
    gate->release();
    first.wait();
    head.wait();
    REQUIRE(first.result->valid());
    REQUIRE(first.result->decoded().size() == 23768);
    REQUIRE(first.result->takeBody().size() == 23768);
    REQUIRE(head.result->takeBody().empty());
    // Reservations survive completion and continue limiting slow HTTP sends.
    REQUIRE(executor.statistics().waiters == 2);
    REQUIRE_FALSE(executor.reserveResponse(1));
    first.result.reset();
    head.result.reset();
    executor.stop();
    executor.join();
    REQUIRE(executor.statistics().jobs == 0);
    REQUIRE(executor.statistics().waiters == 0);
    REQUIRE(executor.statistics().bytes == 0);
}

SCENARIO("Gzip work skips cancelled queued jobs and drains an active decode on stop") {
    auto                    gate = std::make_shared<Gate>();
    Executor                executor({2, 3, 200000, 1000}, {gate});
    Completion              active, queued;
    ex::inplace_stop_source cancellation;
    auto                    bytes = std::string(gzipfixtures::fixed);
    auto                    a     = ex::connect(executor.submit(bytes, 23768), Receiver{active, {}});
    bytes[4]                      = 1;
    auto    b                     = ex::connect(executor.submit(bytes, 23768), Receiver{queued, cancellation.get_token()});
    Finally cleanup{[&] {
        gate->release();
        executor.stop();
        executor.join();
    }};
    ex::start(a);
    gate->wait();
    ex::start(b);
    REQUIRE(executor.statistics().jobs == 2);
    cancellation.request_stop();
    GIVEN("only the queued request is cancelled") {
        gate->release();
        active.wait();
        queued.wait();
        REQUIRE(active.result->valid());
        REQUIRE(queued.stopped);
        REQUIRE(executor.statistics().decodes == 1);
    }
    GIVEN("the server stops while the first decode is running") {
        executor.stop();
        REQUIRE(executor.statistics().jobs == 2);
        gate->release();
        executor.join();
        active.wait();
        queued.wait();
        REQUIRE(active.stopped);
        REQUIRE(queued.stopped);
        REQUIRE(executor.statistics().decodes == 1);
        REQUIRE(executor.statistics().bytes == 0);
    }
}

SCENARIO("Gzip work rejects saturation without starting a worker and preserves decode failures") {
    GIVEN("zero job, waiter, byte or encoded-input capacity") {
        for (const auto limits : {
                 gzip::DecodeLimits{0, 2, 100000, 1000},
                 {2, 0, 100000, 1000},
                 {2, 2,    100, 1000},
                 {2, 2, 100000,  498}
        }) {
            auto       gate = std::make_shared<Gate>();
            Executor   executor(limits, {gate});
            Completion result;
            auto       op = ex::connect(executor.submit(std::string(gzipfixtures::fixed), 23768), Receiver{result, {}});
            ex::start(op);
            result.wait();
            REQUIRE_FALSE(static_cast<bool>(*result.result));
            REQUIRE_FALSE(executor.statistics().workerStarted);
            REQUIRE(executor.statistics().bytes == 0);
        }
    }
    GIVEN("corrupt gzip or a decoded output budget one byte too small") {
        for (const bool corrupt : {false, true}) {
            auto       gate = std::make_shared<Gate>();
            Executor   executor({}, {gate});
            Completion result;
            auto       bytes = std::string(gzipfixtures::fixed);
            if (corrupt)
                bytes[bytes.size() - 8] ^= 1;
            auto    op = ex::connect(executor.submit(bytes, corrupt ? 23768 : 23767), Receiver{result, {}});
            Finally cleanup{[&] {
                gate->release();
                executor.stop();
                executor.join();
            }};
            ex::start(op);
            gate->wait();
            gate->release();
            result.wait();
            REQUIRE_FALSE(result.result->valid());
            REQUIRE(result.result->decoded().empty());
        }
    }
}

SCENARIO("A gated cold gzip decode leaves HTTP and WebSocket upgrades responsive on the I/O thread") {
    const auto                                   port = freePort();
    auto                                         gate = std::make_shared<Gate>();
    Server<Net, NeutralFS, OneJob, GatedDecoder> server("127.0.0.1", port, ".", {}, {gate});
    std::jthread                                 io([&] {
        server.run();
    });
    Net::IoContext                               clients;
    Net::Socket                                  cold(clients), plain(clients), upgrade(clients), saturated(clients), pass(clients), warm(clients);
    Finally                                      cleanup{[&] {
        gate->release();
        server.stop();
        io.join();
        server.drain();
    }};
    send(cold, port, "/asset.js");
    gate->wait();
    REQUIRE(gate->worker != io.get_id());
    send(plain, port, "/plain.txt");
    REQUIRE(receive(plain).ends_with("progress"));
    send(upgrade, port, "/", "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    REQUIRE(receive(upgrade).starts_with("HTTP/1.1 101"));
    send(saturated, port, "/other.js");
    const auto rejected = receive(saturated);
    REQUIRE(rejected.starts_with("HTTP/1.1 503"));
    REQUIRE(rejected.find("Vary: Accept-Encoding") != std::string::npos);
    send(pass, port, "/alias.js", "Accept-Encoding: gzip\r\n");
    const auto encoded = receive(pass);
    REQUIRE(encoded.find("Content-Encoding: x-gzip") != std::string::npos);
    REQUIRE(encoded.ends_with(gzipfixtures::fixed));
    gate->release();
    const auto decoded = receive(cold);
    REQUIRE(decoded.starts_with("HTTP/1.1 200"));
    REQUIRE(decoded.find("Content-Length: 23768") != std::string::npos);
    send(warm, port, "/asset.js", {}, true);
    const auto metadata = receive(warm, true);
    REQUIRE(metadata.find("Content-Length: 23768") != std::string::npos);
    REQUIRE(metadata.ends_with("\r\n\r\n"));
    REQUIRE(server.fallbackDecodeStatistics().decodes == 1);
}

SCENARIO("HTTP stop and disconnected clients cannot outlive the owned gzip worker") {
    const auto                                                 port = freePort();
    auto                                                       gate = std::make_shared<Gate>();
    Server<Net, NeutralFS, DefaultBuffersPolicy, GatedDecoder> server("127.0.0.1", port, ".", {}, {gate});
    std::jthread                                               io([&] {
        server.run();
    });
    Net::IoContext                                             clients;
    Net::Socket                                                client(clients);
    Finally                                                    cleanup{[&] {
        gate->release();
        server.stop();
        if (io.joinable())
            io.join();
        server.drain();
    }};
    send(client, port, "/asset.js");
    gate->wait();
    client.close();
    server.stop();
    io.join();
    REQUIRE(server.fallbackDecodeStatistics().jobs == 1);
    gate->release();
    server.drain();
    REQUIRE(server.fallbackDecodeStatistics().jobs == 0);
    REQUIRE(server.fallbackDecodeStatistics().waiters == 0);
    REQUIRE(server.fallbackDecodeStatistics().bytes == 0);
}

SCENARIO("Gzip coalescing isolates consumer cancellation and different output budgets") {
    auto                    gate = std::make_shared<Gate>();
    Executor                executor({2, 3, 200000, 1000}, {gate});
    Completion              first, second;
    ex::inplace_stop_source cancellation;
    const auto              bytes = std::string(gzipfixtures::fixed);
    GIVEN("one of two consumers of a revision cancels locally") {
        auto    a = ex::connect(executor.submit(bytes, 23768), Receiver{first, cancellation.get_token()});
        auto    b = ex::connect(executor.submit(bytes, 23768, true), Receiver{second, {}});
        Finally cleanup{[&] {
            gate->release();
            executor.stop();
            executor.join();
        }};
        ex::start(a);
        gate->wait();
        ex::start(b);
        cancellation.request_stop();
        gate->release();
        first.wait();
        second.wait();
        REQUIRE(first.stopped);
        REQUIRE(second.result->valid());
        REQUIRE(executor.statistics().decodes == 1);
    }
    GIVEN("a smaller output budget fails before a larger one validates the same bytes") {
        auto    a = ex::connect(executor.submit(bytes, 23767), Receiver{first, {}});
        auto    b = ex::connect(executor.submit(bytes, 23768), Receiver{second, {}});
        Finally cleanup{[&] {
            gate->release();
            executor.stop();
            executor.join();
        }};
        ex::start(a);
        gate->wait();
        ex::start(b);
        REQUIRE(executor.statistics().jobs == 2);
        gate->release();
        first.wait();
        second.wait();
        REQUIRE_FALSE(first.result->valid());
        REQUIRE(second.result->valid());
        REQUIRE(executor.statistics().decodes == 2);
    }
}

SCENARIO("Gzip response leases enforce exact capacity accounting after CPU completion") {
    auto        gate  = std::make_shared<Gate>();
    const auto  bytes = std::string(gzipfixtures::fixed);
    std::string buffer;
    buffer.reserve(23768);
    const auto budget = bytes.capacity() + 1 + 2 * (buffer.capacity() + 1);
    Executor   executor({2, 3, budget, 1000}, {gate});
    Completion first, rejected;
    auto       a = ex::connect(executor.submit(bytes, 23768), Receiver{first, {}});
    auto       b = ex::connect(executor.submit(bytes, 23768, true), Receiver{rejected, {}});
    Finally    cleanup{[&] {
        gate->release();
        executor.stop();
        executor.join();
    }};
    ex::start(a);
    gate->wait();
    REQUIRE(executor.statistics().bytes == budget);
    ex::start(b);
    rejected.wait();
    REQUIRE_FALSE(static_cast<bool>(*rejected.result));
    gate->release();
    first.wait();
    REQUIRE(executor.statistics().bytes == budget);
    REQUIRE_FALSE(executor.reserveResponse(1));
    first.result.reset();
    executor.stop();
    executor.join();
    REQUIRE(executor.statistics().bytes == 0);
}

SCENARIO("Gzip completion resumes on the supplied I/O scheduler") {
    auto           gate = std::make_shared<Gate>();
    Executor       executor({}, {gate});
    Net::IoContext context;
    auto           work = context.make_work_guard();
    Completion     result;
    auto           op = ex::connect(ex::continues_on(executor.submit(std::string(gzipfixtures::fixed), 23768), context.get_scheduler()), Receiver{result, {}});
    std::jthread   io([&] {
        context.run();
    });
    const auto     ioThread = io.get_id();
    Finally        cleanup{[&] {
        gate->release();
        executor.stop();
        executor.join();
        context.stop();
        io.join();
    }};
    ex::start(op);
    gate->wait();
    gate->release();
    result.wait();
    REQUIRE(result.thread == ioThread);
    REQUIRE(result.thread != gate->worker);
    REQUIRE(result.result->valid());
}

SCENARIO("A disconnected gzip requester leaves the shared decode usable by another browser") {
    const auto                                   port = freePort();
    auto                                         gate = std::make_shared<Gate>();
    Server<Net, NeutralFS, OneJob, GatedDecoder> server("127.0.0.1", port, ".", {}, {gate});
    std::jthread                                 io([&] {
        server.run();
    });
    Net::IoContext                               clients;
    Net::Socket                                  lost(clients), plain(clients), next(clients);
    Finally                                      cleanup{[&] {
        gate->release();
        server.stop();
        io.join();
        server.drain();
    }};
    send(lost, port, "/asset.js");
    gate->wait();
    lost.close();
    send(plain, port, "/plain.txt");
    REQUIRE(receive(plain).ends_with("progress"));
    gate->release();
    send(next, port, "/asset.js");
    const auto response = receive(next);
    REQUIRE(response.starts_with("HTTP/1.1 200"));
    REQUIRE(response.find("Content-Length: 23768") != std::string::npos);
    REQUIRE(server.fallbackDecodeStatistics().decodes == 1);
}

SCENARIO("HTTP gzip workers preserve failure statuses and remain lazy on pass-through") {
    const auto port = freePort();
    auto       gate = std::make_shared<Gate>();
    gate->release();
    GIVEN("ordinary and pass-through responses need no worker") {
        Server<Net, NeutralFS, OneJob, GatedDecoder> server("127.0.0.1", port, ".", {}, {gate});
        std::jthread                                 io([&] {
            server.run();
        });
        Net::IoContext                               clients;
        Net::Socket                                  plain(clients), encoded(clients), allocationFailure(clients);
        Finally                                      cleanup{[&] {
            server.stop();
            io.join();
            server.drain();
        }};
        send(allocationFailure, port, "/allocation-failure.js");
        REQUIRE(receive(allocationFailure).starts_with("HTTP/1.1 503"));
        send(plain, port, "/plain.txt");
        REQUIRE(receive(plain).ends_with("progress"));
        send(encoded, port, "/asset.js", "Accept-Encoding: gzip\r\n");
        REQUIRE(receive(encoded).ends_with(gzipfixtures::fixed));
        REQUIRE_FALSE(server.fallbackDecodeStatistics().workerStarted);
    }
    GIVEN("invalid gzip and exceptions from the worker") {
        for (const bool fail : {false, true}) {
            Server<Net, NeutralFS, OneJob, GatedDecoder> server("127.0.0.1", port, ".", {}, {gate, fail});
            std::jthread                                 io([&] {
                server.run();
            });
            Net::IoContext                               clients;
            Net::Socket                                  get(clients), head(clients);
            Finally                                      cleanup{[&] {
                server.stop();
                io.join();
                server.drain();
            }};
            for (auto* client : {&get, &head}) {
                send(*client, port, fail ? "/asset.js" : "/corrupt.js", {}, client == &head);
                const auto response = receive(*client, client == &head);
                REQUIRE(response.starts_with("HTTP/1.1 500"));
                REQUIRE(response.find("Vary: Accept-Encoding") != std::string::npos);
            }
        }
    }
}

namespace {
struct SmallInput : DefaultBuffersPolicy {
    static constexpr std::size_t maxGzipEncodedAssetSize = 498;
};
struct ShrinkingBudget : DefaultBuffersPolicy {
    static inline thread_local std::size_t maxDecodedAssetSize = 23768;
};
}  // namespace

SCENARIO("HTTP rejects oversized fallback input before enqueue while preserving gzip pass-through") {
    RequestHandler<Net, NeutralFS, SmallInput> handler(".");
    Request                                    request;
    request.method = Request::Method::Get;
    request.uri    = "/asset.js";
    REQUIRE(handler.handleRequest(request).statusCode == Response::ok);
    REQUIRE(handler.fallbackCacheStatistics().entries == 1);
    for (const auto method : {Request::Method::Get, Request::Method::Head}) {
        request.method      = method;
        const auto prepared = handler.prepareRequest(request);
        REQUIRE(prepared.response.statusCode == Response::internalServerError);
        REQUIRE_FALSE(prepared.needsDecode);
        REQUIRE(prepared.response.getHeaderValue("Vary") == "Accept-Encoding");
    }
    const auto                         port = freePort();
    Server<Net, NeutralFS, SmallInput> server("127.0.0.1", port);
    std::jthread                       io([&] {
        server.run();
    });
    Net::IoContext                     clients;
    Net::Socket                        fallback(clients), encoded(clients);
    Finally                            cleanup{[&] {
        server.stop();
        io.join();
        server.drain();
    }};
    send(fallback, port, "/asset.js");
    REQUIRE(receive(fallback).starts_with("HTTP/1.1 500"));
    send(encoded, port, "/asset.js", "Accept-Encoding: gzip\r\n");
    REQUIRE(receive(encoded).ends_with(gzipfixtures::fixed));
    REQUIRE_FALSE(server.fallbackDecodeStatistics().workerStarted);
}

SCENARIO("HTTP rechecks the output budget after a suspended worker finishes") {
    const auto oldLimit = ShrinkingBudget::maxDecodedAssetSize;
    Finally    restore{[oldLimit] {
        ShrinkingBudget::maxDecodedAssetSize = oldLimit;
    }};
    ShrinkingBudget::maxDecodedAssetSize                 = 23768;
    auto                                            gate = std::make_shared<Gate>();
    Executor                                        executor({}, {gate});
    RequestHandler<Net, NeutralFS, ShrinkingBudget> handler{"."};
    Request                                         request;
    request.method = Request::Method::Get;
    request.uri    = "/asset.js";
    auto prepared  = handler.prepareRequest(request);
    REQUIRE(prepared.needsDecode);
    Completion result;
    auto       op = ex::connect(executor.submit(std::move(prepared.response.content), prepared.limit), Receiver{result, {}});
    Finally    cleanup{[&] {
        gate->release();
        executor.stop();
        executor.join();
    }};
    ex::start(op);
    gate->wait();
    ShrinkingBudget::maxDecodedAssetSize = 23767;
    gate->release();
    result.wait();
    REQUIRE(handler.finish(std::move(prepared), *result.result).statusCode == Response::internalServerError);
    REQUIRE(handler.fallbackCacheStatistics().entries == 0);
}

SCENARIO("New gzip consumers retry work skipped or interrupted by a decoder exception") {
    for (const bool failed : {false, true}) {
        auto                    gate = std::make_shared<Gate>();
        Executor                executor({4, 4, 400000, 1000}, {gate, false, std::make_shared<std::atomic_bool>(failed)});
        Completion              active, abandoned, retry;
        ex::inplace_stop_source cancellation;
        auto                    bytes = std::string(gzipfixtures::fixed);
        auto                    a     = ex::connect(executor.submit(bytes, 23768), Receiver{active, {}});
        if (!failed)
            bytes[4] = 1;
        auto b = ex::connect(executor.submit(bytes, 23768), Receiver{abandoned, cancellation.get_token()});
        auto c = ex::connect(executor.submit(bytes, 23768), Receiver{retry, {}});
        // Reenter admission during completion: the worker still owns the completed Work.
        auto& first        = failed ? active : abandoned;
        first.onCompletion = [&] {
            ex::start(c);
        };
        Finally cleanup{[&] {
            gate->release();
            executor.stop();
            executor.join();
        }};
        ex::start(a);
        gate->wait();
        if (!failed) {
            ex::start(b);
            cancellation.request_stop();
        }
        gate->release();
        first.wait();
        retry.wait();
        REQUIRE(static_cast<bool>(*retry.result));
        REQUIRE(executor.statistics().decodes == 2);
        REQUIRE(retry.result->valid());
    }
}
