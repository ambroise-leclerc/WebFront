#include <catch2/catch_test_macros.hpp>
#include <tooling/Logger.hpp>
#include <networking/NetworkingMock.hpp>
#include <WebFront.hpp>

#include "Mocks.hpp"

#include <array>
#include <cstring>
#include <list>
#include <string>
#include <type_traits>

using namespace webfront;

namespace {

using TestFilesystem = MockFileSystem<>;

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
