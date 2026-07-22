#include <catch2/catch_test_macros.hpp>
#include <tooling/Logger.hpp>
#include <networking/NetworkingMock.hpp>
#include <WebFront.hpp>

#include "Mocks.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <list>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace webfront;

namespace {

using TestFilesystem = MockFileSystem<>;

struct ResultLinkMock {
    std::vector<std::byte> message;

    void sendFrame(websocket::Frame<networking::NetworkingMock> frame) {
        frame.freeze();
        const auto buffers = frame.toBuffers();
        for (auto buffer = std::next(buffers.begin()); buffer != buffers.end(); ++buffer) {
            const auto* first = static_cast<const std::byte*>(buffer->data());
            message.insert(message.end(), first, first + buffer->size());
        }
    }
};

class WebFrontIoContextMock {
public:
    void run() {}
    void run_one() {}
    void stop() {}
};

class WebFrontResolverMock {
public:
    explicit WebFrontResolverMock(WebFrontIoContextMock) {}

    std::list<networking::EndpointMock> resolve(std::string_view address, std::string_view port) {
        return {networking::EndpointMock{address, port}};
    }
};

class WebFrontAcceptorMock : public networking::SocketBaseMock {
    bool openState = false;

public:
    explicit WebFrontAcceptorMock(WebFrontIoContextMock) {}

    void open(auto protocol) {
        log::debug("WebFrontAcceptorMock::open({})", protocol);
        openState = true;
    }

    void async_accept(std::function<void(std::error_code, networking::SocketMock)>) {}

    bool is_open() const { return openState; }

    void close() { openState = false; }
};

class WebFrontNetworkingMock : public networking::NetworkingMock {
public:
    using Acceptor  = WebFrontAcceptorMock;
    using IoContext = WebFrontIoContextMock;
    using Resolver  = WebFrontResolverMock;
};

struct InitializingFrontend {
    static inline int initializeCalls = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {
        ++initializeCalls;
    }

    static void open(std::string_view, std::string_view) {}
};

struct OpeningFrontend {
    static inline int initializeCalls = 0;
    static inline int openCalls       = 0;
    static inline std::string lastPort;
    static inline std::string lastFile;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {
        ++initializeCalls;
    }

    static void open(std::string_view port, std::string_view file) {
        ++openCalls;
        lastPort = std::string(port);
        lastFile = std::string(file);
    }
};

struct ClosingFrontend {
    static inline int initializeCalls = 0;
    static inline int openCalls       = 0;
    static inline std::string lastPort;
    static inline std::string lastFile;
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};

    static void initialize() {
        ++initializeCalls;
    }

    static void open(std::string_view port, std::string_view file) {
        ++openCalls;
        lastPort = std::string(port);
        lastFile = std::string(file);
    }
};

int freeFunctionForDeductionTest(double value) {
    return static_cast<int>(value);
}

}  // namespace

SCENARIO("Frontend selection aliases") {
    THEN("the default frontend follows the CEF availability") {
        if constexpr (cef::webfrontEmbedCEF)
            REQUIRE(std::is_same_v<frontend::DefaultFrontend, frontend::CEFFrontend>);
        else
            REQUIRE(std::is_same_v<frontend::DefaultFrontend, frontend::DefaultBrowserFrontend>);
    }

    THEN("the convenience alias keeps the selected frontend type") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, OpeningFrontend>;
        REQUIRE(std::is_same_v<typename FrontendWF::FrontendProvider, OpeningFrontend>);
    }
}

SCENARIO("BasicWF initializes a custom frontend once") {
    GIVEN("two BasicWF instances sharing the same frontend type") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;

        WHEN("they are constructed") {
            FrontendWF first("8080");
            FrontendWF second("8081");

            THEN("the frontend initialization runs only once") {
                REQUIRE(InitializingFrontend::initializeCalls == 1);
            }
        }
    }
}

SCENARIO("BasicWF delegates window opening to the selected frontend") {
    GIVEN("a frontend that keeps the server running after open") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, OpeningFrontend>;
        FrontendWF webFront("8123");

        WHEN("openWindow is called") {
            auto action = webFront.openWindow("index.html");

            THEN("the frontend receives the request and keeps the server alive") {
                REQUIRE(action == FrontendWF::WindowAction::none);
                REQUIRE(OpeningFrontend::initializeCalls == 1);
                REQUIRE(OpeningFrontend::openCalls == 1);
                REQUIRE(OpeningFrontend::lastPort == "8123");
                REQUIRE(OpeningFrontend::lastFile == "index.html");
            }
        }
    }

    GIVEN("a frontend that completes when the window session ends") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ClosingFrontend>;
        FrontendWF webFront("9000");

        WHEN("openWindow is called") {
            auto action = webFront.openWindow("react.html");

            THEN("BasicWF requests server shutdown after the frontend returns") {
                REQUIRE(action == FrontendWF::WindowAction::closeWindow);
                REQUIRE(ClosingFrontend::initializeCalls == 1);
                REQUIRE(ClosingFrontend::openCalls == 1);
                REQUIRE(ClosingFrontend::lastPort == "9000");
                REQUIRE(ClosingFrontend::lastFile == "react.html");
            }
        }
    }
}

SCENARIO("Registered C++ function handlers own and invoke their callable") {
    GIVEN("handlers created from temporary callables") {
        int  decodedValue = 0;
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
        FrontendWF webFront("9100");
        auto registeredFunction = [] {};
        webFront.cppFunction<void>("temporary", registeredFunction);
        registeredFunction();

        auto voidHandler = detail::makeCppFunctionHandler<http::DefaultBuffersPolicy, void, int>(
          [&decodedValue](int value) { decodedValue = value; });
        auto returningHandler = detail::makeCppFunctionHandler<http::DefaultBuffersPolicy, int>([] { return 42; });

        std::array<std::byte, 1 + sizeof(double)> encodedNumber{};
        encodedNumber.front() = static_cast<std::byte>(msg::CodedType::number);
        double value          = 42.0;
        std::memcpy(encodedNumber.data() + 1, &value, sizeof(value));

        WHEN("the handlers are invoked after registration") {
            voidHandler(encodedNumber);
            returningHandler(std::span<const std::byte>{});

            THEN("the stored callables remain valid") {
                REQUIRE(decodedValue == 42);
            }
        }
    }
}

SCENARIO("cppFunction deduces a callable's signature from itself") {
    THEN("callable_traits resolves the return type and decayed, owning parameter types") {
        auto voidLambda = [](int) {};
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(voidLambda)>::Return, void>);
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(voidLambda)>::DecayedArgs, std::tuple<int>>);

        auto valueLambda = [](const std::string& text) { return text + "!"; };
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(valueLambda)>::Return, std::string>);
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(valueLambda)>::DecayedArgs, std::tuple<std::string>>);

        auto mutableLambda = [count = 0](int add) mutable { return count += add; };
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(mutableLambda)>::Return, int>);
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(mutableLambda)>::DecayedArgs, std::tuple<int>>);

        REQUIRE(std::is_same_v<detail::callable_traits<decltype(&freeFunctionForDeductionTest)>::Return, int>);
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(&freeFunctionForDeductionTest)>::DecayedArgs, std::tuple<double>>);

        std::function<void(std::string)> stdFunction = [](std::string) {};
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(stdFunction)>::Return, void>);
        REQUIRE(std::is_same_v<detail::callable_traits<decltype(stdFunction)>::DecayedArgs, std::tuple<std::string>>);
    }
}

SCENARIO("Deduced cppFunction registration accepts common callable kinds") {
    GIVEN("a WebFront instance") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
        FrontendWF webFront("9150");

        WHEN("callables are registered without explicit template arguments") {
            int  observed = 0;
            auto voidLambda = [&observed](int value) { observed = value; };
            webFront.cppFunction("onVoid", voidLambda);

            auto valueLambda = [](const std::string& text) { return text + "!"; };
            webFront.cppFunction("onValue", valueLambda);

            auto mutableLambda = [count = 0](int add) mutable { return count += add; };
            webFront.cppFunction("onMutable", mutableLambda);

            webFront.cppFunction("onFreeFunction", &freeFunctionForDeductionTest);

            std::function<void(std::string)> stdFunction = [](std::string) {};
            webFront.cppFunction("onStdFunction", stdFunction);

            THEN("registration compiles and the original callables remain usable") {
                voidLambda(7);
                REQUIRE(observed == 7);
                REQUIRE(valueLambda("hi") == "hi!");
            }
        }
    }
}

SCENARIO("C++ function responders encode values and failures") {
    using Policy = http::DefaultBuffersPolicy;
    using Net    = networking::NetworkingMock;

    GIVEN("a successful result") {
        ResultLinkMock link;
        auto responder = detail::makeCppFunctionResponder<Net, Policy, std::string>(
          [](std::span<const std::byte>) { return std::string{"answer"}; });
        responder({}, link, 42);

        auto result = msg::FunctionReturn<Policy>::castFromRawData(link.message);
        auto payload = result->payload();
        std::string value;
        result->decodeParameter(value, payload);

        THEN("the correlated value is returned") {
            REQUIRE(result->getCallId() == 42);
            REQUIRE(value == "answer");
            REQUIRE(payload.empty());
        }
    }

    GIVEN("a standard exception") {
        ResultLinkMock link;
        auto responder = detail::makeCppFunctionResponder<Net, Policy, void>(
          [](std::span<const std::byte>) { throw std::runtime_error("callback failed"); });
        responder({}, link, 7);

        auto result = msg::FunctionReturn<Policy>::castFromRawData(link.message);
        auto payload = result->payload();
        std::string message;
        result->decodeParameter(message, payload);

        THEN("its useful message is encoded") {
            REQUIRE(message == "callback failed");
        }
    }

    GIVEN("a non-standard exception") {
        ResultLinkMock link;
        auto responder = detail::makeCppFunctionResponder<Net, Policy, void>(
          [](std::span<const std::byte>) { throw 42; });
        responder({}, link, 8);

        auto result = msg::FunctionReturn<Policy>::castFromRawData(link.message);
        auto payload = result->payload();
        std::string message;
        result->decodeParameter(message, payload);

        THEN("a stable fallback message is encoded") {
            REQUIRE(message == "Unknown C++ exception");
        }
    }

    GIVEN("a fire-and-forget call") {
        ResultLinkMock link;
        bool called = false;
        auto responder = detail::makeCppFunctionResponder<Net, Policy, void>(
          [&called](std::span<const std::byte>) { called = true; });
        responder({}, link, 0);

        THEN("the callback runs without sending a result") {
            REQUIRE(called);
            REQUIRE(link.message.empty());
        }
    }
}

SCENARIO("WebFrontConfig has newcomer-friendly defaults") {
    THEN("the defaults are loopback binding, port 9002, and no local document root") {
        WebFrontConfig config;
        REQUIRE(config.address == "127.0.0.1");
        REQUIRE(config.port == "9002");
        REQUIRE(config.documentRoot.empty());
    }
}

SCENARIO("BasicWF supports default construction") {
    GIVEN("a frontend and networking mock") {
        WHEN("a BasicWF is default-constructed") {
            using DefaultCtorWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
            THEN("construction succeeds without specifying port, root, page, frontend, or callbacks") {
                REQUIRE_NOTHROW(DefaultCtorWF{});
            }
        }
    }
}

SCENARIO("BasicWF does not require onUIStarted to be set") {
    // Note: this exercises construction/registration/teardown without onUIStarted. The guarded
    // dispatch itself - onEvent's `if (uiStartedHandler) uiStartedHandler(...)` for the `linked`
    // event - would need a full HTTP-upgrade-then-WebSocket-handshake simulation through the mock
    // networking stack to exercise end-to-end; that infrastructure doesn't exist yet in this test
    // suite, so this guard is otherwise verified by inspection (a plain std::function truthiness
    // check around the call).
    GIVEN("a BasicWF that never registers onUIStarted") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
        WHEN("it is constructed and used") {
            THEN("nothing throws") {
                REQUIRE_NOTHROW([] {
                    FrontendWF webFront("9160");
                    webFront.cppFunction<void>("noop", [] {});
                }());
            }
        }
    }
}
