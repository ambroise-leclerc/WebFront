#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "Version.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace webfront;

SCENARIO("The public version matches the version detected by CMake", "[version]") {
    THEN("consumers and CMake observe the same version") {
        REQUIRE(webfront::version == cmakeProjectVersion);
    }
}

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
    std::size_t                    run_one() {
        return 0;
    }
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

struct RetryingFrontend {
    static inline int initializeCalls = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {
        if (++initializeCalls == 1)
            throw std::runtime_error("frontend initialization failed");
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
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    STATIC_REQUIRE(frontend::FrontendType<frontend::NativeWebViewFrontend>);
    STATIC_REQUIRE(std::is_same_v<webfront::WebFrontWithFrontend<frontend::NativeWebViewFrontend>::FrontendProvider, frontend::NativeWebViewFrontend>);
#endif
#if defined(_WIN32)
    STATIC_REQUIRE(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WebView2Frontend>);
#elif defined(__APPLE__)
    STATIC_REQUIRE(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WKWebViewFrontend>);
#elif defined(__linux__)
    STATIC_REQUIRE(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WebKitGTKFrontend>);
#endif
    STATIC_REQUIRE(std::is_same_v<webfront::WebFront::FrontendProvider, frontend::DefaultBrowserFrontend>);
    STATIC_REQUIRE(frontend::FrontendType<frontend::WKWebViewFrontend>);
    STATIC_REQUIRE(frontend::WKWebViewFrontend::action == frontend::Action::closeServerAfterOpen);
    STATIC_REQUIRE(frontend::FrontendType<frontend::WebKitGTKFrontend>);
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

#if (defined(_WIN32) && !defined(WEBFRONT_EMBED_WEBVIEW2)) || (defined(__APPLE__) && !defined(WEBFRONT_EMBED_WKWEBVIEW))                                       \
    || (defined(__linux__) && !defined(WEBFRONT_EMBED_WEBKITGTK))
SCENARIO("Selecting the native web view without support preserves the platform diagnostic") {
    #if defined(_WIN32)
    const auto diagnostic = "WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON";
    #elif defined(__APPLE__)
    const auto diagnostic = "WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON";
    #else
    const auto diagnostic = "WebKitGTKFrontend is unavailable. Configure on Linux with -DWEBFRONT_EMBED_WEBKITGTK=ON";
    #endif
    THEN("opening names the platform adapter's enabling option") {
        REQUIRE_THROWS_WITH(frontend::NativeWebViewFrontend::open("9200", "index.html"), diagnostic);
    }
}
#endif

#if !defined(WEBFRONT_EMBED_WKWEBVIEW) || !defined(__APPLE__)
SCENARIO("Selecting WKWebView without support fails clearly") {
    GIVEN("a build without macOS WKWebView support") {
        THEN("initialization and opening name the enabling option") {
            REQUIRE_THROWS_WITH(frontend::WKWebViewFrontend::initialize(),
                                "WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON");
            REQUIRE_THROWS_WITH(frontend::WKWebViewFrontend::open("9200", "index.html"),
                                "WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON");
            REQUIRE_THROWS_AS(frontend::WKWebViewFrontend::open("127.0.0.1", "9200", "index.html", ContentPolicy{}), std::runtime_error);
        }
    }
}
#endif

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

#ifndef WEBFRONT_EMBED_WEBKITGTK
SCENARIO("Selecting WebKitGTK without support fails clearly") {
    THEN("initialization names the enabling option") {
        REQUIRE_THROWS_WITH(frontend::WebKitGTKFrontend::initialize(),
            "WebKitGTKFrontend is unavailable. Configure on Linux with -DWEBFRONT_EMBED_WEBKITGTK=ON");
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
                    REQUIRE_THROWS_AS(frontend::CEFFrontend::open("127.0.0.1", "9200", "index.html", ContentPolicy{}), std::runtime_error);
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

SCENARIO("Failed frontend initialization is retried and success is shared across application types") {
    GIVEN("a frontend whose first initialization fails") {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, RetryingFrontend>;
        using OtherApp = BasicWFWithFrontend<WebFrontNetworkingMock, fs::DefaultFS, RetryingFrontend>;
        WHEN("construction is retried and another application type uses the frontend") {
            THEN("construction propagates the error, retries once, and reuses successful initialization") {
                REQUIRE_THROWS_WITH(App("9200"), "frontend initialization failed");
                REQUIRE(RetryingFrontend::initializeCalls == 1);
                App retry("9201");
                REQUIRE(RetryingFrontend::initializeCalls == 2);
                OtherApp other("9202");
                REQUIRE(RetryingFrontend::initializeCalls == 2);
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

struct FallbackNativeWindow {
    static inline bool        displayAvailable = true;
    static inline int         openCalls        = 0;
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};
    static void               initialize() {
        if (!displayAvailable)
            throw std::runtime_error("cannot open a display; set DISPLAY or WAYLAND_DISPLAY");
    }
    static void open(std::string_view, std::string_view) { ++openCalls; }
};

struct FallbackBrowser {
    static inline int initializeCalls = 0;
    static inline int openCalls = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() { ++initializeCalls; }
    static void                       open(std::string_view, std::string_view) { ++openCalls; }
};

SCENARIO("Native window or browser, decided at run time") {
    using Fallback = frontend::NativeOrBrowserFrontendFor<FallbackNativeWindow, FallbackBrowser>;
    STATIC_REQUIRE(frontend::FrontendType<Fallback>);
    STATIC_REQUIRE(frontend::FrontendType<frontend::NativeOrBrowserFrontend>);
    STATIC_REQUIRE(std::is_same_v<webfront::WebFrontWithFrontend<frontend::NativeOrBrowserFrontend>::FrontendProvider, frontend::NativeOrBrowserFrontend>);

    GIVEN("a native window that can open") {
        FallbackNativeWindow::displayAvailable = true;
        FallbackNativeWindow::openCalls        = 0;
        FallbackBrowser::openCalls             = 0;
        FallbackBrowser::initializeCalls       = 0;
        Fallback::initialize();
        THEN("the native window is used and closing it stops the server") {
            REQUIRE(FallbackBrowser::initializeCalls == 0);
            REQUIRE(Fallback::usesNativeWindow());
            REQUIRE_FALSE(Fallback::fallbackReason().has_value());
            REQUIRE(Fallback::closeServerAfterOpen());
            Fallback::open("127.0.0.1", "9200", "index.html");
            REQUIRE(FallbackNativeWindow::openCalls == 1);
            REQUIRE(FallbackBrowser::openCalls == 0);
        }
    }
    GIVEN("a session without a display") {
        FallbackNativeWindow::displayAvailable = false;
        FallbackNativeWindow::openCalls        = 0;
        FallbackBrowser::openCalls             = 0;
        FallbackBrowser::initializeCalls       = 0;
        std::ostringstream captured;
        auto* const        previous = std::cerr.rdbuf(captured.rdbuf());
        Fallback::initialize();
        std::cerr.rdbuf(previous);
        THEN("the browser is used, the reason is kept and printed, and the server keeps running") {
            REQUIRE_FALSE(Fallback::usesNativeWindow());
            REQUIRE(FallbackBrowser::initializeCalls == 1);
            REQUIRE(Fallback::fallbackReason().value_or("").find("display") != std::string::npos);
            REQUIRE(captured.str().find("opening the system browser instead of a native window") != std::string::npos);
            REQUIRE_FALSE(Fallback::closeServerAfterOpen());
            Fallback::open("127.0.0.1", "9200", "index.html");
            REQUIRE(FallbackNativeWindow::openCalls == 0);
            REQUIRE(FallbackBrowser::openCalls == 1);
        }
        THEN("an application falling back does not stop the server after open()") {
            using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, Fallback>;
            App app("9203");
            const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
            app.show();
            REQUIRE(FallbackBrowser::openCalls == 1);
            REQUIRE(WebFrontIoContextMock::stopCalls.load() == previousStopCalls);
        }
    }
#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
    GIVEN("the real WebKitGTK engine and no DISPLAY or WAYLAND_DISPLAY") {
        // A child process: GTK may already be initialized in this one, and the environment must stay untouched.
        const pid_t child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            unsetenv("DISPLAY");
            unsetenv("WAYLAND_DISPLAY");
            std::cerr.rdbuf(nullptr);
            frontend::NativeOrBrowserFrontend::initialize();
            const bool fellBack = !frontend::NativeOrBrowserFrontend::usesNativeWindow()
                                  && frontend::NativeOrBrowserFrontend::fallbackReason().value_or("").find("display") != std::string::npos;
            _exit(fellBack ? 0 : 1);
        }
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        THEN("the frontend falls back to the browser and names the missing display") {
            REQUIRE(WIFEXITED(status));
            REQUIRE(WEXITSTATUS(status) == 0);
        }
    }
#endif
}

SCENARIO("The native engine availability is a constant") {
    STATIC_REQUIRE(std::is_same_v<decltype(frontend::nativeWebViewAvailable), const bool>);
#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
    STATIC_REQUIRE(frontend::nativeWebViewAvailable);
#elif defined(__linux__)
    STATIC_REQUIRE_FALSE(frontend::nativeWebViewAvailable);
    THEN("without an engine the fallback frontend always uses the browser") {
        std::ostringstream captured;
        auto* const        previous = std::cerr.rdbuf(captured.rdbuf());
        frontend::NativeOrBrowserFrontend::initialize();
        std::cerr.rdbuf(previous);
        REQUIRE_FALSE(frontend::NativeOrBrowserFrontend::usesNativeWindow());
        REQUIRE(frontend::NativeOrBrowserFrontend::fallbackReason().value_or("").find("WEBFRONT_EMBED_WEBKITGTK") != std::string::npos);
    }
#endif
}

SCENARIO("The embedded frontend is declared in every configuration") {
    STATIC_REQUIRE(std::is_same_v<decltype(embeddedFrontendAvailable), const bool>);
    STATIC_REQUIRE(embeddedFrontendAvailable == frontend::nativeWebViewAvailable);
    STATIC_REQUIRE(std::is_class_v<EmbeddedWebFront>);
    STATIC_REQUIRE(std::is_same_v<EmbeddedWebFront, WebFrontWithFrontend<detail::EmbeddedFrontendType>>);
    STATIC_REQUIRE(frontend::FrontendType<detail::EmbeddedFrontendType>);
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    STATIC_REQUIRE(std::is_same_v<detail::EmbeddedFrontendType, frontend::NativeWebViewFrontend> == embeddedFrontendAvailable);
#endif
    if constexpr (!embeddedFrontendAvailable) {
        THEN("constructing it throws std::runtime_error naming the missing engine") {
            REQUIRE_THROWS_WITH(EmbeddedWebFront{}, Catch::Matchers::ContainsSubstring("no embedded web view engine"));
            REQUIRE_THROWS_AS(EmbeddedWebFront{}, std::runtime_error);
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

namespace {
// Preserve the legacy empty-options call despite the new ContentPolicy overload.
static_assert(requires { frontend::WebView2Frontend::open("127.0.0.1", "9002", "index.html", {}); });

struct PolicyFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static inline ContentPolicy       received;
    static inline std::string         receivedAddress;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) {
        throw std::runtime_error("Legacy overload selected");
    }
    static void open(std::string_view address, std::string_view, std::string_view, const ContentPolicy& policy) {
        receivedAddress = address;
        received        = policy;
    }
};
}  // namespace

SCENARIO("WebFront passes per-application content rules to policy-aware frontends") {
    using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, PolicyFrontend>;
    App app(WebFrontConfig{
        .address       = "127.0.0.2",
        .contentPolicy = {.navigationAllowedOrigins = {"https://docs.example"}, .popups = PopupPolicy::systemBrowser, .devToolsEnabled = false}
    });
    REQUIRE_NOTHROW(app.openWindow("index.html"));
    THEN("the configured address reaches the preferred overload") {
        REQUIRE(PolicyFrontend::receivedAddress == "127.0.0.2");
    }
    THEN("the navigation allow-list is preserved") {
        REQUIRE(PolicyFrontend::received.navigationAllowedOrigins == std::vector<std::string>{"https://docs.example"});
    }
    THEN("popup and DevTools settings are preserved independently") {
        REQUIRE(PolicyFrontend::received.popups == PopupPolicy::systemBrowser);
        REQUIRE_FALSE(PolicyFrontend::received.devToolsEnabled);
    }
}

SCENARIO("Ephemeral ports are bound atomically and forwarded to the frontend") {
    using App = BasicWFWithFrontend<NetProvider, TestFilesystem, AddressFrontend>;
    App first{WebFrontConfig{.port = "0"}};
    App second{WebFrontConfig{.port = "0"}};
    first.openWindow("index.html");
    const auto firstPort = AddressFrontend::lastPort;
    second.openWindow("index.html");
    REQUIRE(firstPort != "0");
    REQUIRE(AddressFrontend::lastPort != "0");
    REQUIRE(firstPort != AddressFrontend::lastPort);
    REQUIRE(std::stoi(firstPort) > 0);
    REQUIRE(std::stoi(AddressFrontend::lastPort) > 0);
    REQUIRE(first.port() == firstPort);
    REQUIRE(second.port() == AddressFrontend::lastPort);
    GIVEN("a legacy provider without a bound-endpoint query") {
        using MockApp = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, AddressFrontend>;
        MockApp mock{WebFrontConfig{.port = "0"}};
        REQUIRE(mock.port() == "0");
    }
}

SCENARIO("Sending a DOM batch through a lost UI link throws ConnectionError") {
    using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
    GIVEN("a UI handle whose connection is absent from the application") {
        App           app("9161");
        App::UI       ui(app, 42, 1);
        msg::DomBatch batch(7);
        batch.add({msg::DomOp::text, 1, 0, 0, "unreachable"});
        THEN("sending fails synchronously without queuing a frame") {
            REQUIRE_THROWS_AS(ui.sendDomBatch(batch), ConnectionError);
            REQUIRE(ui.pendingWrites() == 0);
        }
    }
}

SCENARIO("A UI is an assignable value identifying one browser of one application") {
    using App = BasicWF<networking::NetworkingMock, fs::DefaultFS>;
    GIVEN("an application over the networking mock") {
        App app(WebFrontConfig{.port = "0"});
        THEN("a UI is an assignable value usable in containers") {
            static_assert(std::is_copy_assignable_v<App::UI>);
            static_assert(std::is_nothrow_move_assignable_v<App::UI>);
            static_assert(std::equality_comparable<App::UI>);
            static_assert(!std::is_copy_constructible_v<App> && !std::is_move_constructible_v<App>);
            static_assert(!std::is_copy_assignable_v<App> && !std::is_move_assignable_v<App>);
            std::vector<App::UI> clients{App::UI(app, 1, 1), App::UI(app, 2, 1), App::UI(app, 3, 1)};
            std::erase_if(clients, [](const App::UI& ui) { return ui.id() == 2; });
            REQUIRE(clients.size() == 2);
            CHECK(clients[0] == App::UI(app, 1, 1));
            CHECK(clients[0] != clients[1]);
            CHECK(clients[1].id() == 3);
            std::optional<App::UI> current;
            current = clients[1];
            CHECK(current->id() == 3);
        }
        THEN("a UI of another application is never equal, even with the same identifier") {
            struct Application { } first, second;
            using StubUI = BasicUI<Application>;
            CHECK(StubUI(first, 1, 1) != StubUI(second, 1, 1));
            CHECK(StubUI(first, 1, 1) != StubUI(first, 1, 2));  // same identifier, later connection
            StubUI ui(first, 1, 1);
            ui = StubUI(second, 1, 1);
            CHECK(ui == StubUI(second, 1, 1));
        }
        THEN("a UI has no default state and a reassigned one acts on its new value") {
            static_assert(!std::is_default_constructible_v<App::UI>);
            App::UI       ui(app, 1, 1);
            const App::UI gone(app, 42, 1);
            ui = gone;
            CHECK(ui == gone);
            CHECK_THROWS_AS(ui.addScript("1"), ConnectionError);
            CHECK(ui.pendingWrites() == 0);
            CHECK_NOTHROW(ui.jsFunction("console.log")("dropped"));
            auto result = ui.jsFunction<int>("Math.abs")(-1);
            CHECK_THROWS_WITH(result.get(), "Browser connection closed");
            CHECK_NOTHROW(ui.writesAtMost(0));
            App::UI moved(std::move(ui));
            CHECK(moved.id() == 42);
            CHECK_THROWS_AS(moved.addScript("1"), ConnectionError);
        }
    }
}

namespace {
struct DomNoTimerNet : testing::InjectableNetworking {
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

SCENARIO("DOM readiness explains a missing network timer while the ordinary bridge remains usable", "[dom]") {
    using Socket = testing::InjectableSocket;
    Socket::reset();
    DomNoTimerNet::accepted = false;
    BasicWF<DomNoTimerNet, fs::DefaultFS> app{WebFrontConfig{.port = "0"}};
    app.cppFunction("echo", [] {
        return std::string{"bridge stays available"};
    });
    const std::string upgrade = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    Socket::receive(std::as_bytes(std::span(upgrade)));
    msg::Handshake handshake;
    Socket::receive(testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
    REQUIRE(app.connectedBrowsers() == 1);
    auto call = [](std::string name, msg::CallId id) {
        msg::FunctionCall<> message;
        message.setCallId(id);
        websocket::Frame<DomNoTimerNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
        message.encodeParameter(name, frame);
        frame.freeze();
        Socket::receive(testing::clientFrame(testing::messagePayload(frame)));
    };
    call("wfDom.ready", 21);
    REQUIRE(Socket::wrote("DOM API requires Net::ResumeAfter"));
    call("echo", 22);
    REQUIRE(Socket::wrote("bridge stays available"));
    REQUIRE(app.connectedBrowsers() == 1);
    app.shutdown();
}
