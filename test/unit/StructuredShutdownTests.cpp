#include "InlineTaskScope.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/system/DefaultFS.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace ex = webfront::networking::ex;

struct ShutdownTrace {
    int liveSockets        = 0;
    int foreignCompletions = 0;
};

// I/O cancellation is posted, not completed from the thread requesting stop.
// One unrelated completion remains ready after the tracked operations finish.
struct ShutdownContext {
    static inline ShutdownContext*    current{};
    std::shared_ptr<ShutdownTrace>    trace = std::make_shared<ShutdownTrace>();
    std::deque<std::function<void()>> ready;
    bool                              foreignQueued = false;
    bool                              stopped       = false;
    ShutdownContext() {
        current = this;
    }
    void stop() {
        stopped = true;
    }
    void restart() {
        stopped = false;
    }
    std::size_t run_one() {
        if (stopped || ready.empty())
            return 0;
        auto next = std::move(ready.front());
        ready.pop_front();
        next();
        if (!foreignQueued) {
            foreignQueued = true;
            ready.push_back([state = trace] {
                ++state->foreignCompletions;
            });
        }
        return 1;
    }
    void run() {
        while (!stopped && !ready.empty())
            run_one();
    }
};

struct ShutdownSocket : webfront::networking::SocketMock {
    std::shared_ptr<ShutdownTrace> trace;
    ShutdownSocket() : trace(ShutdownContext::current->trace) {
        ++trace->liveSockets;
    }
    ShutdownSocket(ShutdownSocket&& other) noexcept : trace(std::move(other.trace)) { }
    ~ShutdownSocket() {
        if (trace)
            --trace->liveSockets;
    }
    std::size_t write_some(auto buffer, std::error_code&) {
        return buffer.size();
    }
};

template <typename T>
struct ShutdownSender {
    using sender_concept        = ex::sender_t;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(T), ex::set_stopped_t()>;
    bool produce                = false;

    template <typename Receiver>
    struct Operation {
        using StopToken = ex::stop_token_of_t<ex::env_of_t<Receiver>>;
        struct Stop {
            Operation* operation;
            void       operator()() noexcept {
                ShutdownContext::current->ready.push_back([op = operation] {
                    op->callback.reset();
                    ex::set_stopped(std::move(op->receiver));
                });
            }
        };
        Receiver                                                receiver;
        bool                                                    produce;
        std::optional<ex::stop_callback_for_t<StopToken, Stop>> callback;
        Operation(Receiver value, bool emit) : receiver(std::move(value)), produce(emit) { }
        void start() noexcept {
            if (produce) {
                ex::set_value(std::move(receiver), T{});
                return;
            }
            callback.emplace(ex::get_stop_token(ex::get_env(receiver)), Stop{this});
        }
    };
    template <typename Receiver>
    auto connect(Receiver receiver) const {
        return Operation<Receiver>(std::move(receiver), produce);
    }
};

struct ShutdownAcceptor : webfront::networking::SocketBaseMock {
    bool accepted = false;
    explicit ShutdownAcceptor(ShutdownContext&) { }
    void open(auto) { }
    bool is_open() const {
        return true;
    }
};

struct ShutdownNet : webfront::networking::NetworkingMock {
    using IoContext = ShutdownContext;
    using Acceptor  = ShutdownAcceptor;
    using Socket    = ShutdownSocket;
    static auto Accept(Acceptor& acceptor) {
        return ShutdownSender<Socket>{!std::exchange(acceptor.accepted, true)};
    }
    static auto Receive(Socket&, MutableBuffer) {
        return ShutdownSender<std::size_t>{};
    }
    static auto Send(Socket&, const auto&) {
        return ex::just(std::size_t{});
    }
};
}  // namespace

SCENARIO("Server drain joins its pending connection without draining unrelated work") {
    std::shared_ptr<ShutdownTrace> trace;
    GIVEN("a server with an accepted socket and a pending read") {
        {
            webfront::http::Server<ShutdownNet, webfront::fs::DefaultFS> server("127.0.0.1", "0");
            trace = ShutdownContext::current->trace;
            REQUIRE(trace->liveSockets == 1);
            WHEN("shutdown cancellation is queued on the context") {
                server.drain();
                THEN("the connection is destroyed before drain returns") {
                    CHECK(trace->liveSockets == 0);
                }
                AND_THEN("join does not require unrelated context work to finish") {
                    CHECK(trace->foreignCompletions == 0);
                }
            }
        }
        THEN("no connection outlives the server") {
            CHECK(trace->liveSockets == 0);
        }
    }
}

namespace {
webfront::networking::Task failingTask(int failure) {
    if (failure == 1)
        throw std::runtime_error("tracked failure");
    if (failure == 2)
        throw 42;
    co_return;
}
}  // namespace
SCENARIO("Tracked task errors are logged and do not prevent joining") {
    namespace log = webfront::log;
    std::vector<std::string> lines;
    log::setLogLevel(log::Error);
    auto handle = log::addSinks([&](std::string_view text) {
        lines.emplace_back(text);
    });
    GIVEN("tasks that fail with standard and nonstandard exceptions") {
        webfront::testing::InlineTaskScope scope;
        WHEN("they are spawned into a scope") {
            webfront::networking::Spawn(scope, ex::inline_scheduler{}, failingTask(0));
            webfront::networking::Spawn(scope, ex::inline_scheduler{}, failingTask(1));
            webfront::networking::Spawn(scope, ex::inline_scheduler{}, failingTask(2));
            log::removeSinks(handle);
            THEN("both errors are logged and the scope can be drained") {
                CHECK(lines.size() == 2);
                CHECK(std::ranges::any_of(lines, [](const auto& line) {
                    return line.ends_with("Network task failed: tracked failure");
                }));
                CHECK(std::ranges::any_of(lines, [](const auto& line) {
                    return line.ends_with("Network task failed");
                }));
            }
        }
    }
}
