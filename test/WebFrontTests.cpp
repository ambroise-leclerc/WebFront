#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <tooling/Logger.hpp>
#include <networking/NetworkingMock.hpp>
#include <WebFront.hpp>

#include "Mocks.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <list>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
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
    static inline std::atomic<int> stopCalls{0};
    void run() {}
    void run_one() {}
    void stop() { ++stopCalls; }
    void restart() {}
};

class WebFrontAcceptorMock : public networking::SocketBaseMock {
    bool openState = false;

public:
    explicit WebFrontAcceptorMock(WebFrontIoContextMock) {}

    void open(auto protocol) {
        log::debug("WebFrontAcceptorMock::open({})", protocol);
        openState = true;
    }

    bool is_open() const { return openState; }

    void close() { openState = false; }
};

class WebFrontNetworkingMock : public networking::NetworkingMock {
public:
    using Acceptor  = WebFrontAcceptorMock;
    using IoContext = WebFrontIoContextMock;
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

struct FailingFrontend {
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};
    static void initialize() {}
    static void open(std::string_view, std::string_view) { throw std::runtime_error("frontend startup failed"); }
};

struct ThreadRecordingFrontend {
    static inline std::thread::id openThread;
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};
    static void initialize() {}
    static void open(std::string_view, std::string_view) { openThread = std::this_thread::get_id(); }
};

struct FailingNonBlockingFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void initialize() {}
    static void open(std::string_view, std::string_view) { throw std::runtime_error("launcher failed"); }
};

struct AddressFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static inline std::string         lastAddress;
    static inline std::string         lastPort;
    static inline std::string         lastFile;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
    static void                       open(std::string_view address, std::string_view port, std::string_view file) {
        lastAddress = address;
        lastPort    = port;
        lastFile    = file;
    }
};

int freeFunctionForDeductionTest(double value) {
    return static_cast<int>(value);
}

}  // namespace

SCENARIO("Frontend selection aliases") {
    STATIC_REQUIRE(frontend::FrontendType<frontend::WebView2Frontend>);
    STATIC_REQUIRE(frontend::WebView2Frontend::action == frontend::Action::closeServerAfterOpen);
    THEN("the default frontend is always the system browser, regardless of CEF availability") {
        REQUIRE(std::is_same_v<frontend::DefaultFrontend, frontend::DefaultBrowserFrontend>);
    }

    THEN("the convenience alias keeps the selected frontend type") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, OpeningFrontend>;
        REQUIRE(std::is_same_v<typename FrontendWF::FrontendProvider, OpeningFrontend>);
    }
}

#if !defined(WEBFRONT_EMBED_WEBVIEW2) || !defined(_WIN32)
SCENARIO("Selecting WebView2 without support fails clearly") {
    GIVEN("a build without Windows WebView2 support") {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, frontend::WebView2Frontend>;
        App app("9200");
        THEN("opening the frontend explains which option to enable") {
            REQUIRE_THROWS_WITH(app.openWindow("index.html"),
                "WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON");
        }
    }
}
#endif

SCENARIO("Selecting CEFFrontend without CEF support fails clearly") {
    GIVEN("a build without CEF support") {
        WHEN("a CEF-fronted WebFront is constructed") {
            THEN("construction throws a clear diagnostic instead of silently doing nothing") {
                if constexpr (!cef::webfrontEmbedCEF) {
                    using CEFWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, frontend::CEFFrontend>;
                    REQUIRE_THROWS_AS(CEFWF("9200"), std::runtime_error);
                }
            }
        }
    }

    GIVEN("a build without CEF support") {
        WHEN("the CEF frontend is opened directly") {
            THEN("the diagnostic explains how to enable CEF") {
                if constexpr (!cef::webfrontEmbedCEF) {
                    REQUIRE_THROWS_WITH(frontend::CEFFrontend::open("9200", "index.html"),
                                        "webfront::frontend::CEFFrontend requires CEF support, but this build was configured with "
                                        "WEBFRONT_EMBED_CEF=OFF. Reconfigure with -DWEBFRONT_EMBED_CEF=ON to use the embedded CEF frontend.");
                }
            }
        }
    }
}

SCENARIO("CEF subprocess exits preserve their status and diagnostic") {
    GIVEN("a CEF subprocess that completed successfully") {
        const cef::CEFSubprocessExit exit{0};

        THEN("the frontend can propagate a successful process status") {
            REQUIRE(exit.exit_code() == 0);
        }
    }

    GIVEN("a CEF subprocess that failed") {
        const cef::CEFSubprocessExit exit{7};

        THEN("the frontend can propagate the failure status and report the exit request") {
            REQUIRE(exit.exit_code() == 7);
            REQUIRE(std::string_view(exit.what()) == "CEF subprocess should exit");
        }
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

SCENARIO("A frontend startup failure stops the server before propagating") {
    GIVEN("an embedded frontend that fails to open") {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, FailingFrontend>;
        App app("9200");
        const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
        WHEN("the application opens the frontend") {
            THEN("show requests server shutdown before propagating the diagnostic") {
                REQUIRE_THROWS_WITH(app.show(), "frontend startup failed");
                REQUIRE(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
            }
        }
    }
}

SCENARIO("The frontend contract: open() runs on the calling thread and a closing frontend stops the server") {
    GIVEN("an embedded frontend that records the thread it is opened on") {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ThreadRecordingFrontend>;
        App app("9201");
        const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
        WHEN("the application shows the interface") {
            app.show();
            THEN("open() ran on the thread that called show(), not on the server thread") {
                REQUIRE(ThreadRecordingFrontend::openThread == std::this_thread::get_id());
            }
            THEN("show() returned after the server was asked to stop") {
                REQUIRE(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
            }
        }
    }
}

SCENARIO("The frontend contract: a failing open() stops the server whatever its action") {
    GIVEN("a frontend that only launches an external program and fails to do so") {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, FailingNonBlockingFrontend>;
        App app("9202");
        const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
        THEN("the exception propagates and the server is stopped") {
            REQUIRE_THROWS_WITH(app.show(), "launcher failed");
            REQUIRE(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
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
        const int  previousOpenCalls = ClosingFrontend::openCalls;  // other scenarios share this frontend
        FrontendWF webFront("9000");

        WHEN("openWindow is called") {
            auto action = webFront.openWindow("react.html");

            THEN("BasicWF requests server shutdown after the frontend returns") {
                REQUIRE(action == FrontendWF::WindowAction::closeWindow);
                REQUIRE(ClosingFrontend::initializeCalls == 1);
                REQUIRE(ClosingFrontend::openCalls == previousOpenCalls + 1);
                REQUIRE(ClosingFrontend::lastPort == "9000");
                REQUIRE(ClosingFrontend::lastFile == "react.html");
            }
        }
    }
}

SCENARIO("BasicWF forwards the configured address to address-aware frontends") {
    using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, AddressFrontend>;
    App app{
        WebFrontConfig{.address = "127.0.0.2", .port = "9010"}
    };
    app.openWindow("index.html?x=1#result");
    REQUIRE(AddressFrontend::lastAddress == "127.0.0.2");
    REQUIRE(AddressFrontend::lastPort == "9010");
    REQUIRE(AddressFrontend::lastFile == "index.html?x=1#result");
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

SCENARIO("show opens the default page and returns when the window session ends") {
    GIVEN("a default-constructed application whose frontend completes after open") {
        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ClosingFrontend>;
        FrontendWF webFront;
        const int  previousOpenCalls = ClosingFrontend::openCalls;

        WHEN("show is called without arguments") {
            webFront.show();

            THEN("the default page is opened on the default port") {
                REQUIRE(ClosingFrontend::openCalls == previousOpenCalls + 1);
                REQUIRE(ClosingFrontend::lastPort == "9002");
                REQUIRE(ClosingFrontend::lastFile == "index.html");
            }
        }

        WHEN("show is called with a page") {
            webFront.show("greeter.html");

            THEN("that page is opened") {
                REQUIRE(ClosingFrontend::lastFile == "greeter.html");
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

namespace {

// The single string parameter of the return message a responder sent.
std::string sentString(const ResultLinkMock& link) {
    auto        result  = msg::FunctionReturn<http::DefaultBuffersPolicy>::castFromRawData(link.message);
    auto        payload = result->payload();
    std::string value;
    result->decodeParameter(value, payload);
    if (result->getParametersCount() != 1 || !payload.empty())
        throw std::runtime_error("The return message does not hold exactly one parameter");
    return value;
}

template <typename R>
std::string respondWith(R value) {
    ResultLinkMock link;
    auto           responder = detail::makeCppFunctionResponder<networking::NetworkingMock, http::DefaultBuffersPolicy, R>([value](std::span<const std::byte>) {
        return value;
    });
    responder({}, link, 9);
    return sentString(link);
}

}  // namespace

SCENARIO("C++ results too long for the wire reject the JavaScript Promise") {
    const std::string overlong(65536, 'x');

    GIVEN("a callback returning a string over 65535 bytes") {
        THEN("a single well-formed exception is sent") {
            REQUIRE(respondWith(overlong) == "WebFront strings cannot exceed 65535 bytes");
        }
    }
    GIVEN("a callback returning such a string inside a tuple") {
        THEN("the partly encoded result is replaced by a single well-formed exception") {
            REQUIRE(respondWith(std::tuple<int, std::string>{1, overlong}) == "WebFront strings cannot exceed 65535 bytes");
        }
    }
}

namespace {

struct CallerMock {
    int id;
};

// The call data of a single small string parameter, as the browser encodes it.
std::vector<std::byte> smallStringData(std::string_view text) {
    std::vector<std::byte> data{static_cast<std::byte>(msg::CodedType::smallString), static_cast<std::byte>(text.size())};
    for (const char c : text)
        data.push_back(static_cast<std::byte>(c));
    return data;
}

}  // namespace

SCENARIO("A C++ callback can receive the browser that called it") {
    using Policy = http::DefaultBuffersPolicy;

    GIVEN("a handler whose callable takes the caller before its JavaScript parameters") {
        auto       handler = detail::makeCppFunctionHandler<Policy, std::string, std::string>([](const CallerMock& caller, const std::string& text) {
            return std::to_string(caller.id) + ":" + text;
        });
        const auto data    = smallStringData("hi");

        THEN("the caller is passed first and only the remaining parameters are decoded") {
            REQUIRE(handler(data, CallerMock{3}) == "3:hi");
        }
    }
    GIVEN("a callable taking the caller by mutable reference, adapted to its declared type") {
        auto handler = detail::makeCppFunctionHandler<Policy, std::string, std::string>(
            detail::adaptCaller<CallerMock&>([](CallerMock& caller, const std::string& text) {
                caller.id += 1;
                return std::to_string(caller.id) + ":" + text;
            }));
        CallerMock caller{3};
        const auto data = smallStringData("hi");

        THEN("it receives the caller itself") {
            REQUIRE(handler(data, caller) == "4:hi");
            REQUIRE(caller.id == 4);
        }
    }
    GIVEN("a responder for such a handler") {
        ResultLinkMock link;
        auto           handler   = detail::makeCppFunctionHandler<Policy, std::string>([](CallerMock caller) {
            return std::to_string(caller.id);
        });
        auto           responder = detail::makeCppFunctionResponder<networking::NetworkingMock, Policy, std::string>(std::move(handler));
        responder({}, link, 5, CallerMock{11});

        THEN("the caller reaches the callable and the result is returned") {
            REQUIRE(sentString(link) == "11");
        }
    }
}

// Only a first parameter of the application's UI type, with any reference or const, is the caller.
using CallerTestUI = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>::UI;
static_assert(detail::takes_caller<CallerTestUI, CallerTestUI, std::string>::value);
static_assert(detail::takes_caller<CallerTestUI, const CallerTestUI&>::value);
static_assert(!detail::takes_caller<CallerTestUI, std::string, CallerTestUI>::value);
static_assert(!detail::takes_caller<CallerTestUI>::value);

SCENARIO("A UI first parameter selects the caller form of cppFunction") {
    using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
    using AppUI      = FrontendWF::UI;

    GIVEN("an application") {
        FrontendWF webFront("9161");
        THEN("the caller may be taken by value or by const reference") {
            REQUIRE_NOTHROW(webFront.cppFunction("byValue", [](AppUI) {}));
            REQUIRE_NOTHROW(webFront.cppFunction("byConstReference", [](const AppUI&) {}));
        }
        THEN("the caller may be taken by mutable or rvalue reference") {
            REQUIRE_NOTHROW(webFront.cppFunction("byReference", [](AppUI&) {}));
            REQUIRE_NOTHROW(webFront.cppFunction("byRvalueReference", [](AppUI&&) {}));
        }
        THEN("caller callbacks register through deduction and explicit types") {
            REQUIRE_NOTHROW(webFront.cppFunction("deduced", [](AppUI, std::string) {}));
            REQUIRE_NOTHROW(webFront.template cppFunction<int, const AppUI&, double>("explicit", [](const AppUI&, double) {
                return 1;
            }));
        }
        THEN("browser subscriptions can be taken, and released after the application") {
            UISubscription subscription;
            {
                FrontendWF scoped("9162");
                subscription = scoped.subscribeUIStarted([](AppUI) {});
                REQUIRE(subscription.active());
            }
            REQUIRE_NOTHROW(subscription.reset());
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
