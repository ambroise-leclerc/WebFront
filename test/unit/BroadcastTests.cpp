// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Broadcasting to every connected browser, and the threads of callbacks and browser calls (#401).
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/WebFront.hpp>

#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#else
    #include <poll.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace webfront;
using namespace std::chrono_literals;

namespace {
const std::string upgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

// The payload of a browser's call to a C++ function, without a reply (call identifier 0).
template <typename Net>
std::vector<std::byte> callPayload(const std::string& name) {
    msg::FunctionCall<>   message;
    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    frame.freeze();
    return testing::messagePayload(frame);
}

struct OneBrowserNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};
}  // namespace

SCENARIO("A broadcast keeps only the latest call for a browser with too many pending writes", "[broadcast]") {
    using Socket = testing::InjectableSocket;
    Socket::reset();
    OneBrowserNet::accepted = false;
    BasicWF<OneBrowserNet, fs::DefaultFS> app{WebFrontConfig{.port = "0"}};
    auto                                  latest = app.broadcast("onState", {.maxPendingWrites = 1});
    auto                                  every  = app.broadcast("onEvent");
    Socket::receive(std::as_bytes(std::span(upgradeRequest)));
    REQUIRE(app.connectedBrowsers() == 1);

    GIVEN("a browser whose handshake has not completed") {
        latest(std::string{"before-handshake"});
        THEN("it is not started yet and receives nothing") {
            REQUIRE_FALSE(Socket::wrote("before-handshake"));
        }
    }
    GIVEN("a started browser whose network completes writes only when the test says so") {
        msg::Handshake handshake;
        Socket::receive(testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
        Socket::deferWrites();
        Socket::completeDeferredWrites();  // the handshake acknowledgement

        WHEN("three states are broadcast while the first one is still being written") {
            latest(std::string{"state-1"});
            latest(std::string{"state-2"});
            latest(std::string{"state-3"});
            THEN("only the first is written before the network completes it") {
                REQUIRE(Socket::wrote("state-1"));
                REQUIRE_FALSE(Socket::wrote("state-3"));
            }
            AND_WHEN("the network completes the writes") {
                while (Socket::completeDeferredWrites() != 0) {}
                THEN("the latest state follows and the intermediate one was skipped") {
                    REQUIRE(Socket::wrote("state-3"));
                    REQUIRE_FALSE(Socket::wrote("state-2"));
                }
                THEN("a later state is written at once") {
                    latest(std::string{"state-4"});
                    REQUIRE(Socket::wrote("state-4"));
                }
            }
        }
        WHEN("events are broadcast without a limit while the first one is still being written") {
            every(std::string{"event-1"});
            every(std::string{"event-2"});
            every(std::string{"event-3"});
            while (Socket::completeDeferredWrites() != 0) {}
            THEN("every one is written") {
                REQUIRE(Socket::wrote("event-1"));
                REQUIRE(Socket::wrote("event-2"));
                REQUIRE(Socket::wrote("event-3"));
            }
        }
        Socket::deferWrites(false);
        while (Socket::completeDeferredWrites() != 0) {}
    }
    app.shutdown();
    Socket::reset();
}

SCENARIO("A broadcast limit must allow at least one pending write", "[broadcast]") {
    BasicWF<OneBrowserNet, fs::DefaultFS> app{WebFrontConfig{.port = "0"}};
    REQUIRE_THROWS_AS(app.broadcast("onState", {.maxPendingWrites = 0}), std::invalid_argument);
}

namespace {
using Stdnet = networking::TCPStdnet;

void waitUntil(auto condition) {
    for (int i = 0; i < 500 && !condition(); ++i)
        std::this_thread::sleep_for(10ms);
}

// A browser connected over loopback: it upgrades, completes the bridge handshake and records what it receives.
class Browser {
public:
    explicit Browser(std::string_view port) : socket(context) {
        socket.connect(Stdnet::Resolve("127.0.0.1", port));
        socket.send(std::as_bytes(std::span(upgradeRequest)));
        REQUIRE(receiveUntil("\r\n\r\n"));
        const std::array<unsigned char, 8>
            handshake{0x82, 0x82, 0, 0, 0, 0, static_cast<unsigned char>(msg::Command::handshake), static_cast<unsigned char>(msg::JSEndian::little)};
        socket.send(Stdnet::ConstBuffer(handshake.data(), handshake.size()));
    }

    void call(const std::string& name) {
        const auto frame = testing::clientFrame(callPayload<Stdnet>(name));
        socket.send(Stdnet::ConstBuffer(frame.data(), frame.size()));
    }

    /// True once the server sent these bytes, false after the deadline.
    bool receiveUntil(std::string_view text, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (received.find(text) == std::string::npos) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                return false;
            ::pollfd descriptor{};
            descriptor.fd     = socket.native_handle();
            descriptor.events = POLLIN;
#ifdef _WIN32
            const int ready = ::WSAPoll(&descriptor, 1, static_cast<int>(remaining));
#else
            const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
#endif
            if (ready <= 0)
                return false;
            std::array<char, 4096> buffer{};
            const auto             size = socket.receive(Stdnet::Buffer(buffer));
            if (size == 0)
                return false;
            received.append(buffer.data(), size);
        }
        return true;
    }

private:
    Stdnet::IoContext context;
    Stdnet::Socket    socket;
    std::string       received;
};

// What the callbacks observed: the threads they ran on, and how many ran at once.
struct Observed {
    std::mutex                   mutex;
    std::vector<std::thread::id> threads;
    std::atomic<int>             running{0};
    std::atomic<int>             maxRunning{0};
    std::atomic<int>             started{0};
    std::atomic<int>             calls{0};
    std::atomic<int>             closed{0};

    void record() {
        const auto now = ++running;
        for (auto max = maxRunning.load(); now > max && !maxRunning.compare_exchange_weak(max, now);) {}
        std::this_thread::sleep_for(1ms);  // overlap with a callback another thread might be running
        {
            std::scoped_lock lock{mutex};
            threads.push_back(std::this_thread::get_id());
        }
        --running;
    }
};
}  // namespace

SCENARIO("Callbacks run one at a time on the server thread, and browser calls are made from any thread", "[broadcast]") {
    using App = BasicWF<Stdnet, fs::DefaultFS>;
    App        app{WebFrontConfig{.port = "0"}};
    Observed   observed;
    std::mutex applicationLock;  // also taken by a callback
    app.onUIStarted([&](App::UI) {
        observed.record();
        ++observed.started;
    });
    auto closedSubscription = app.subscribeUIClosed([&](App::UI) {
        observed.record();
        ++observed.closed;
    });
    app.cppFunction("report", [&](App::UI ui) {
        observed.record();
        std::scoped_lock lock{applicationLock};
        ui.jsFunction("reported")(std::string{"reported-ack"});
        ++observed.calls;
    });
    std::promise<void> unblock;
    auto               unblocked = unblock.get_future().share();
    std::atomic<bool>  blocking{false};
    app.cppFunction("block", [&] {
        blocking = true;
        unblocked.wait();
    });
    std::thread server([&] {
        app.run();
    });
    const auto  serverThread = server.get_id();

    auto first  = std::make_unique<Browser>(app.port());
    auto second = std::make_unique<Browser>(app.port());
    waitUntil([&] {
        return observed.started == 2;
    });
    REQUIRE(observed.started == 2);

    WHEN("another thread broadcasts a state") {
        auto onState = app.broadcast("siliconForge.onState");
        onState(std::string{"broadcast-state"});
        THEN("every started browser receives it") {
            REQUIRE(first->receiveUntil("broadcast-state"));
            REQUIRE(second->receiveUntil("broadcast-state"));
        }
    }
    WHEN("another thread calls each browser through forEachUI") {
        std::vector<WebLinkId> visited;
        app.forEachUI([&](App::UI ui) {
            visited.push_back(ui.id());
            ui.jsFunction("each")(std::string{"each-ui-" + std::to_string(ui.id())});
        });
        THEN("each started browser is visited once and receives its call") {
            REQUIRE(visited.size() == 2);
            REQUIRE(first->receiveUntil("each-ui-"));
            REQUIRE(second->receiveUntil("each-ui-"));
        }
    }
    WHEN("both browsers call C++ while another thread broadcasts under a lock the callback also takes") {
        auto onState = app.broadcast("siliconForge.onState", {.maxPendingWrites = 2});
        for (int i = 0; i < 20; ++i) {
            first->call("report");
            second->call("report");
            std::scoped_lock lock{applicationLock};
            onState(std::string{"locked-state"});
            app.forEachUI([](App::UI ui) {
                ui.jsFunction("locked")(std::string{"locked-call"});
            });
        }
        waitUntil([&] {
            return observed.calls == 40;
        });
        THEN("no call deadlocks and the callbacks never overlap") {
            REQUIRE(observed.calls == 40);
            REQUIRE(observed.maxRunning == 1);
            REQUIRE(first->receiveUntil("reported-ack"));
            REQUIRE(second->receiveUntil("locked-call"));
        }
    }
    WHEN("a callback blocks the server thread") {
        first->call("block");
        waitUntil([&] {
            return blocking.load();
        });
        REQUIRE(blocking);
        auto       calls  = std::async(std::launch::async, [&] {
            auto onState = app.broadcast("siliconForge.onState", {.maxPendingWrites = 1});
            onState(std::string{"while-blocked"});
            onState(std::string{"while-blocked"});
            app.forEachUI([](App::UI ui) {
                ui.jsFunction("blocked")(std::string{"while-blocked"});
                static_cast<void>(ui.pendingWrites());
            });
        });
        const auto status = calls.wait_for(5s);
        unblock.set_value();
        THEN("browser calls from another thread return without waiting for it") {
            REQUIRE(status == std::future_status::ready);
            REQUIRE(second->receiveUntil("while-blocked"));
        }
    }
    if (blocking == false)
        unblock.set_value();

    WHEN("a browser disconnects") {
        second.reset();
        waitUntil([&] {
            return observed.closed == 1;
        });
        THEN("its closure is reported, and a broadcast reaches the remaining browser") {
            REQUIRE(observed.closed == 1);
            app.broadcast("siliconForge.onState", {.maxPendingWrites = 1})(std::string{"after-close"});
            REQUIRE(first->receiveUntil("after-close"));
        }
    }
    first.reset();
    second.reset();
    waitUntil([&] {
        return app.connectedBrowsers() == 0;
    });
    app.stop();
    server.join();
    app.shutdown();
    THEN("every callback ran on the server thread") {
        std::scoped_lock lock{observed.mutex};
        REQUIRE_FALSE(observed.threads.empty());
        REQUIRE(std::ranges::all_of(observed.threads, [&](std::thread::id id) {
            return id == serverThread;
        }));
    }
}
