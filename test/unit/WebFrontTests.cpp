// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-05-31
//
// WebFront — tests/unit
// Role: The application object: construction, callbacks, browsers, calls in both directions and shutdown.
// Thread: ui-loop | Alloc: unrestricted
// LLM: largest application test; uses link and io context mocks

#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "Version.hpp"
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/PtyPage.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
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

using namespace webfront::bdd;

#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace webfront;

const Scenario publicVersion{"The public version matches the version detected by CMake", {"version"}, [] {
                                 then("consumers and CMake observe the same version", [&] {
                                     require(webfront::version == cmakeProjectVersion);
                                 });
                             }};

const Scenario ptyPageAuthorization{
    "PTY page requires explicit opt-in, a loopback bind and the application Origin",
    [] {
        then("registration is refused by default and opt-in rejects wildcard and non-loopback binds", [] {
            WebFront app{WebFrontConfig{.port = "0"}};
            requireThrows<std::logic_error>([&] {
                registerPtyPage(app, {});
            });
            require(!app.hasCppFunction("webfront.pty.open"));
            for (const auto* address : {"0.0.0.0", "::", "192.0.2.1", "localhost", "0:0:0:0:0:0:0:1"}) {
                requireThrows<std::invalid_argument>([&] {
                    WebFront rejected{
                        WebFrontConfig{.address = address, .port = "0", .contentPolicy = {.allowMissingOrigin = false}, .allowPtyPages = true}
                    };
                });
            }
        });
        then("configuration rejects a bridge policy that would allow missing or foreign origins", [] {
            for (const auto& policy : {
                     ContentPolicy{},
                     ContentPolicy{.allowAnyBridgeOrigin = true, .allowMissingOrigin = false},
                     ContentPolicy{.bridgeAllowedOrigins = {"http://foreign.example"}, .allowMissingOrigin = false}
            }) {
                requireThrows<std::invalid_argument>([&] {
                    WebFront app{
                        WebFrontConfig{.port = "0", .contentPolicy = policy, .allowPtyPages = true}
                    };
                });
            }
        });
        then("the upgrade handler accepts the application's own Origin and refuses missing or foreign Origin", [] {
            using namespace webfront::http;
            const auto config = webfront::detail::checkedPtyPageConfig({.contentPolicy = {.allowMissingOrigin = false}, .allowPtyPages = true});
            RequestHandler<networking::NetworkingMock, MockFileSystem<>> handler{
                ".",
                webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002/", config.contentPolicy}
            };
            const auto upgrade = [&](std::string_view origin) {
                std::string input = "GET / HTTP/1.1\r\nHost: 127.0.0.1:9002\r\nUpgrade: websocket\r\n"
                                    "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
                input            += origin;
                input            += "\r\n";
                Request request;
                require(request.parseSomeData(input.begin(), input.end()));
                return handler.handleRequest(request).statusCode;
            };
            require(upgrade("") == Response::forbidden);
            require(upgrade("Origin: http://foreign.example\r\n") == Response::forbidden);
            require(upgrade("Origin: http://127.0.0.1:9002\r\n") == Response::switchingProtocols);
            require(webfront::detail::checkedPtyPageConfig({.address = "::1", .contentPolicy = {.allowMissingOrigin = false}, .allowPtyPages = true}).address
                    == "::1");
        });
    }};

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
    void                           run() {}
    std::size_t                    run_one() {
        return 0;
    }
    void stop() {
        ++stopCalls;
    }
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

    bool is_open() const {
        return openState;
    }

    void close() {
        openState = false;
    }
};

class WebFrontNetworkingMock : public networking::NetworkingMock {
public:
    using Acceptor  = WebFrontAcceptorMock;
    using IoContext = WebFrontIoContextMock;
};

struct InitializingFrontend {
    static inline int                 initializeCalls = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {
        ++initializeCalls;
    }

    static void open(std::string_view, std::string_view) {}
};

struct RetryingFrontend {
    static inline int                 initializeCalls = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {
        if (++initializeCalls == 1)
            throw std::runtime_error("frontend initialization failed");
    }

    static void open(std::string_view, std::string_view) {}
};

struct OpeningFrontend {
    static inline int                 initializeCalls = 0;
    static inline int                 openCalls       = 0;
    static inline std::string         lastPort;
    static inline std::string         lastFile;
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
    static inline int                 initializeCalls = 0;
    static inline int                 openCalls       = 0;
    static inline std::string         lastPort;
    static inline std::string         lastFile;
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
    static void                       initialize() {}
    static void                       open(std::string_view, std::string_view) {
        throw std::runtime_error("frontend startup failed");
    }
};

struct ThreadRecordingFrontend {
    static inline std::thread::id     openThread;
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};
    static void                       initialize() {}
    static void                       open(std::string_view, std::string_view) {
        openThread = std::this_thread::get_id();
    }
};

struct FailingNonBlockingFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() {}
    static void                       open(std::string_view, std::string_view) {
        throw std::runtime_error("launcher failed");
    }
};

struct AddressFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static inline std::string         lastAddress;
    static inline std::string         lastPort;
    static inline std::string         lastFile;
    static void                       initialize() {}
    static void                       open(std::string_view, std::string_view) {}
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

const Scenario frontendSelection{
    "Frontend selection aliases",
    [] {
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
        static_assert(frontend::FrontendType<frontend::NativeWebViewFrontend>);
        static_assert(std::is_same_v<webfront::WebFrontWithFrontend<frontend::NativeWebViewFrontend>::FrontendProvider, frontend::NativeWebViewFrontend>);
#endif
#if defined(_WIN32)
        static_assert(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WebView2Frontend>);
#elif defined(__APPLE__)
        static_assert(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WKWebViewFrontend>);
#elif defined(__linux__)
        static_assert(std::is_same_v<frontend::NativeWebViewFrontend, frontend::WebKitGTKFrontend>);
#endif
        static_assert(std::is_same_v<webfront::WebFront::FrontendProvider, frontend::DefaultBrowserFrontend>);
        static_assert(frontend::FrontendType<frontend::WKWebViewFrontend>);
        static_assert(frontend::WKWebViewFrontend::action == frontend::Action::closeServerAfterOpen);
        static_assert(frontend::FrontendType<frontend::WebKitGTKFrontend>);
        static_assert(frontend::FrontendType<frontend::WebView2Frontend>);
        static_assert(frontend::WebView2Frontend::action == frontend::Action::closeServerAfterOpen);
        then("the default frontend is always the system browser, regardless of CEF availability", [&] {
            require(std::is_same_v<frontend::DefaultFrontend, frontend::DefaultBrowserFrontend>);
        });

        then("the convenience alias keeps the selected frontend type", [&] {
            using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, OpeningFrontend>;
            require(std::is_same_v<typename FrontendWF::FrontendProvider, OpeningFrontend>);
        });
    }};

#if (defined(_WIN32) && !defined(WEBFRONT_EMBED_WEBVIEW2)) || (defined(__APPLE__) && !defined(WEBFRONT_EMBED_WKWEBVIEW))                                       \
    || (defined(__linux__) && !defined(WEBFRONT_EMBED_WEBKITGTK))
const Scenario selectingNative{"Selecting the native web view without support preserves the platform diagnostic", [] {
    #if defined(_WIN32)
                                   const auto diagnostic = "WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON";
    #elif defined(__APPLE__)
    const auto diagnostic = "WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON";
    #else
    const auto diagnostic = "WebKitGTKFrontend is unavailable. Configure on Linux with -DWEBFRONT_EMBED_WEBKITGTK=ON";
    #endif
                                   then("opening names the platform adapter's enabling option", [&] {
                                       requireThrows(
                                           [&] {
                                               frontend::NativeWebViewFrontend::open("9200", "index.html");
                                           },
                                           equals(diagnostic));
                                   });
                               }};
#endif

const Scenario selectingFrontend{
    "Selecting a frontend the build does not support fails clearly",
    [] {
#if !defined(WEBFRONT_EMBED_WKWEBVIEW) || !defined(__APPLE__)
        then("WKWebView's initialization and opening name the enabling option", [&] {
            requireThrows(
                [&] {
                    frontend::WKWebViewFrontend::initialize();
                },
                equals("WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON"));
            requireThrows(
                [&] {
                    frontend::WKWebViewFrontend::open("9200", "index.html");
                },
                equals("WKWebViewFrontend is unavailable. Configure on macOS with -DWEBFRONT_EMBED_WKWEBVIEW=ON"));
            requireThrows<std::runtime_error>([&] {
                frontend::WKWebViewFrontend::open("127.0.0.1", "9200", "index.html", ContentPolicy{});
            });
        });
#endif
#if !defined(WEBFRONT_EMBED_WEBVIEW2) || !defined(_WIN32)
        then("opening WebView2 explains which option to enable", [&] {
            using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, frontend::WebView2Frontend>;
            App app("9200");
            requireThrows(
                [&] {
                    app.openWindow("index.html");
                },
                equals("WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON"));
        });
#endif
#ifndef WEBFRONT_EMBED_WEBKITGTK
        then("WebKitGTK's initialization names the enabling option", [&] {
            requireThrows(
                [&] {
                    frontend::WebKitGTKFrontend::initialize();
                },
                equals("WebKitGTKFrontend is unavailable. Configure on Linux with -DWEBFRONT_EMBED_WEBKITGTK=ON"));
        });
#endif
        if constexpr (!cef::webfrontEmbedCEF) {
            then("constructing a CEF-fronted application throws instead of silently doing nothing, and opening CEF explains how to "
                 "enable it",
                 [&] {
                     using CEFWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, frontend::CEFFrontend>;
                     requireThrows<std::runtime_error>([&] {
                         CEFWF("9200");
                     });
                     requireThrows(
                         [&] {
                             frontend::CEFFrontend::open("9200", "index.html");
                         },
                         equals("webfront::frontend::CEFFrontend requires CEF support, but this build was configured with "
                                "WEBFRONT_EMBED_CEF=OFF. Reconfigure with -DWEBFRONT_EMBED_CEF=ON to use the embedded CEF frontend."));
                     requireThrows<std::runtime_error>([&] {
                         frontend::CEFFrontend::open("127.0.0.1", "9200", "index.html", ContentPolicy{});
                     });
                 });
        }
    }};

const Scenario cEFSubprocess{"CEF subprocess exits preserve their status and diagnostic", [] {
                                 then("a subprocess that completed successfully propagates its status, and a failed one its status and the exit request", [&] {
                                     require(cef::CEFSubprocessExit{0}.exit_code() == 0);
                                     const cef::CEFSubprocessExit failed{7};
                                     require(failed.exit_code() == 7);
                                     require(std::string_view(failed.what()) == "CEF subprocess should exit");
                                 });
                             }};

const Scenario frontendInitialized{"A frontend is initialized once per process, and a failed initialization is retried", [] {
                                       given("two BasicWF instances sharing the same frontend type", [&] {
                                           using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                                           FrontendWF first("8080");
                                           FrontendWF second("8081");
                                           then("the frontend initialization runs only once", [&] {
                                               require(InitializingFrontend::initializeCalls == 1);
                                           });
                                       });
                                       given("a frontend whose first initialization fails", [&] {
                                           using App      = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, RetryingFrontend>;
                                           using OtherApp = BasicWFWithFrontend<WebFrontNetworkingMock, fs::DefaultFS, RetryingFrontend>;
                                           when("construction is retried and another application type uses the frontend", [&] {
                                               then("construction propagates the error, retries once, and reuses successful initialization", [&] {
                                                   requireThrows(
                                                       [&] {
                                                           App("9200");
                                                       },
                                                       equals("frontend initialization failed"));
                                                   require(RetryingFrontend::initializeCalls == 1);
                                                   App retry("9201");
                                                   require(RetryingFrontend::initializeCalls == 2);
                                                   OtherApp other("9202");
                                                   require(RetryingFrontend::initializeCalls == 2);
                                               });
                                           });
                                       });
                                   }};

const Scenario frontendContract{"The frontend contract: a failing open() stops the server before propagating, whatever its action", [] {
                                    given("an embedded frontend that fails to open", [&] {
                                        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, FailingFrontend>;
                                        App        app("9200");
                                        const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
                                        then("show requests server shutdown before propagating the diagnostic", [&] {
                                            requireThrows(
                                                [&] {
                                                    app.show();
                                                },
                                                equals("frontend startup failed"));
                                            require(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
                                        });
                                    });
                                    given("a frontend that only launches an external program and fails to do so", [&] {
                                        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, FailingNonBlockingFrontend>;
                                        App        app("9202");
                                        const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
                                        then("the exception propagates and the server is stopped", [&] {
                                            requireThrows(
                                                [&] {
                                                    app.show();
                                                },
                                                equals("launcher failed"));
                                            require(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
                                        });
                                    });
                                }};

const Scenario frontendContractThread{"The frontend contract: open() runs on the calling thread and a closing frontend stops the server", [] {
                                          given("an embedded frontend that records the thread it is opened on", [&] {
                                              using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ThreadRecordingFrontend>;
                                              App        app("9201");
                                              const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
                                              when("the application shows the interface", [&] {
                                                  app.show();
                                                  then("open() ran on the thread that called show(), not on the server thread", [&] {
                                                      require(ThreadRecordingFrontend::openThread == std::this_thread::get_id());
                                                  });
                                                  then("show() returned after the server was asked to stop", [&] {
                                                      require(WebFrontIoContextMock::stopCalls.load() > previousStopCalls);
                                                  });
                                              });
                                          });
                                      }};

struct FallbackNativeWindow {
    static inline bool                displayAvailable = true;
    static inline int                 openCalls        = 0;
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};
    static void                       initialize() {
        if (!displayAvailable)
            throw std::runtime_error("cannot open a display; set DISPLAY or WAYLAND_DISPLAY");
    }
    static void open(std::string_view, std::string_view) {
        ++openCalls;
    }
};

struct FallbackBrowser {
    static inline int                 initializeCalls = 0;
    static inline int                 openCalls       = 0;
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() {
        ++initializeCalls;
    }
    static void open(std::string_view, std::string_view) {
        ++openCalls;
    }
};

const Scenario nativeWindowBrowser{
    "Native window or browser, decided at run time",
    [] {
        using Fallback = frontend::NativeOrBrowserFrontendFor<FallbackNativeWindow, FallbackBrowser>;
        static_assert(frontend::FrontendType<Fallback>);
        static_assert(frontend::FrontendType<frontend::NativeOrBrowserFrontend>);
        static_assert(std::is_same_v<webfront::WebFrontWithFrontend<frontend::NativeOrBrowserFrontend>::FrontendProvider, frontend::NativeOrBrowserFrontend>);

        given("a native window that can open", [&] {
            FallbackNativeWindow::displayAvailable = true;
            FallbackNativeWindow::openCalls        = 0;
            FallbackBrowser::openCalls             = 0;
            FallbackBrowser::initializeCalls       = 0;
            Fallback::initialize();
            then("the native window is used and closing it stops the server", [&] {
                require(FallbackBrowser::initializeCalls == 0);
                require(Fallback::usesNativeWindow());
                require(!Fallback::fallbackReason().has_value());
                require(Fallback::closeServerAfterOpen());
                Fallback::open("127.0.0.1", "9200", "index.html");
                require(FallbackNativeWindow::openCalls == 1);
                require(FallbackBrowser::openCalls == 0);
            });
        });
        given("a session without a display", [&] {
            FallbackNativeWindow::displayAvailable = false;
            FallbackNativeWindow::openCalls        = 0;
            FallbackBrowser::openCalls             = 0;
            FallbackBrowser::initializeCalls       = 0;
            std::ostringstream captured;
            auto* const        previous = std::cerr.rdbuf(captured.rdbuf());
            Fallback::initialize();
            std::cerr.rdbuf(previous);
            then("the browser is used, the reason is kept and printed, and the server keeps running", [&] {
                require(!Fallback::usesNativeWindow());
                require(FallbackBrowser::initializeCalls == 1);
                require(Fallback::fallbackReason().value_or("").find("display") != std::string::npos);
                require(captured.str().find("opening the system browser instead of a native window") != std::string::npos);
                require(!Fallback::closeServerAfterOpen());
                Fallback::open("127.0.0.1", "9200", "index.html");
                require(FallbackNativeWindow::openCalls == 0);
                require(FallbackBrowser::openCalls == 1);
            });
            then("an application falling back does not stop the server after open()", [&] {
                using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, Fallback>;
                App        app("9203");
                const auto previousStopCalls = WebFrontIoContextMock::stopCalls.load();
                app.show();
                require(FallbackBrowser::openCalls == 1);
                require(WebFrontIoContextMock::stopCalls.load() == previousStopCalls);
            });
        });
#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
        given("the real WebKitGTK engine and no DISPLAY or WAYLAND_DISPLAY", [&] {
            // A child process: GTK may already be initialized in this one, and the environment must stay untouched.
            const pid_t child = fork();
            require(child >= 0);
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
            require(waitpid(child, &status, 0) == child);
            then("the frontend falls back to the browser and names the missing display", [&] {
                require(WIFEXITED(status));
                require(WEXITSTATUS(status) == 0);
            });
        });
#endif
    }};

const Scenario nativeEngine{"The native engine availability is a constant", [] {
                                static_assert(std::is_same_v<decltype(frontend::nativeWebViewAvailable), const bool>);
#if defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK)
                                static_assert(frontend::nativeWebViewAvailable);
#elif defined(__linux__)
    static_assert(!(frontend::nativeWebViewAvailable));
    then("without an engine the fallback frontend always uses the browser", [&] {
        std::ostringstream captured;
        auto* const        previous = std::cerr.rdbuf(captured.rdbuf());
        frontend::NativeOrBrowserFrontend::initialize();
        std::cerr.rdbuf(previous);
        require(!frontend::NativeOrBrowserFrontend::usesNativeWindow());
        require(frontend::NativeOrBrowserFrontend::fallbackReason().value_or("").find("WEBFRONT_EMBED_WEBKITGTK") != std::string::npos);
    });
#endif
                            }};

const Scenario embeddedFrontendDeclared{
    "The embedded frontend is declared in every configuration",
    [] {
        static_assert(std::is_same_v<decltype(embeddedFrontendAvailable), const bool>);
#ifdef WEBFRONT_EMBED_CEF
        static_assert(embeddedFrontendAvailable);
        static_assert(std::is_same_v<detail::EmbeddedFrontendType,
                                     std::conditional_t<frontend::nativeWebViewAvailable, frontend::NativeWebViewFrontend, frontend::CEFFrontend>>);
#else
        static_assert(embeddedFrontendAvailable == frontend::nativeWebViewAvailable);
#endif
        static_assert(std::is_class_v<EmbeddedWebFront>);
        static_assert(std::is_same_v<EmbeddedWebFront, WebFrontWithFrontend<detail::EmbeddedFrontendType>>);
        static_assert(frontend::FrontendType<detail::EmbeddedFrontendType>);
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    #ifndef WEBFRONT_EMBED_CEF
        static_assert(std::is_same_v<detail::EmbeddedFrontendType, frontend::NativeWebViewFrontend> == embeddedFrontendAvailable);
    #endif
#endif
        if constexpr (!embeddedFrontendAvailable) {
            then("constructing it throws std::runtime_error naming the missing engine", [&] {
                requireThrows(
                    [&] {
                        EmbeddedWebFront{};
                    },
                    contains("no embedded web view engine"));
                requireThrows<std::runtime_error>([&] {
                    EmbeddedWebFront{};
                });
            });
        }
    }};

const Scenario basicWFDelegates{"BasicWF delegates window opening to the selected frontend", [] {
                                    given("a frontend that keeps the server running after open", [&] {
                                        using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, OpeningFrontend>;
                                        FrontendWF webFront("8123");

                                        when("openWindow is called", [&] {
                                            auto action = webFront.openWindow("index.html");

                                            then("the frontend receives the request and keeps the server alive", [&] {
                                                require(action == FrontendWF::WindowAction::none);
                                                require(OpeningFrontend::initializeCalls == 1);
                                                require(OpeningFrontend::openCalls == 1);
                                                require(OpeningFrontend::lastPort == "8123");
                                                require(OpeningFrontend::lastFile == "index.html");
                                            });
                                        });
                                    });

                                    given("a frontend that completes when the window session ends", [&] {
                                        using FrontendWF             = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ClosingFrontend>;
                                        const int  previousOpenCalls = ClosingFrontend::openCalls;  // other scenarios share this frontend
                                        FrontendWF webFront("9000");

                                        when("openWindow is called", [&] {
                                            auto action = webFront.openWindow("react.html");

                                            then("BasicWF requests server shutdown after the frontend returns", [&] {
                                                require(action == FrontendWF::WindowAction::closeWindow);
                                                require(ClosingFrontend::initializeCalls == 1);
                                                require(ClosingFrontend::openCalls == previousOpenCalls + 1);
                                                require(ClosingFrontend::lastPort == "9000");
                                                require(ClosingFrontend::lastFile == "react.html");
                                            });
                                        });
                                    });

                                    given("a frontend that takes the configured address", [&] {
                                        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, AddressFrontend>;
                                        App app{
                                            WebFrontConfig{.address = "127.0.0.2", .port = "9010"}
                                        };
                                        app.openWindow("index.html?x=1#result");
                                        then("it receives the address, the port and the page with its query and fragment", [&] {
                                            require(AddressFrontend::lastAddress == "127.0.0.2");
                                            require(AddressFrontend::lastPort == "9010");
                                            require(AddressFrontend::lastFile == "index.html?x=1#result");
                                        });
                                    });
                                }};

const Scenario registeredC{"Registered C++ function handlers own and invoke their callable", [] {
                               given("handlers created from temporary callables", [&] {
                                   int decodedValue = 0;
                                   using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                                   FrontendWF webFront("9100");
                                   auto       registeredFunction = [] {};
                                   webFront.cppFunction<void>("temporary", registeredFunction);
                                   registeredFunction();

                                   auto voidHandler      = detail::makeCppFunctionHandler<http::DefaultBuffersPolicy, void, int>([&decodedValue](int value) {
                                       decodedValue = value;
                                   });
                                   auto returningHandler = detail::makeCppFunctionHandler<http::DefaultBuffersPolicy, int>([] {
                                       return 42;
                                   });

                                   std::array<std::byte, 1 + sizeof(double)> encodedNumber{};
                                   encodedNumber.front() = static_cast<std::byte>(msg::CodedType::number);
                                   double value          = 42.0;
                                   std::memcpy(encodedNumber.data() + 1, &value, sizeof(value));

                                   when("the handlers are invoked after registration", [&] {
                                       voidHandler(encodedNumber);
                                       returningHandler(std::span<const std::byte>{});

                                       then("the stored callables remain valid", [&] {
                                           require(decodedValue == 42);
                                       });
                                   });
                               });
                           }};

const Scenario applicationServes{"An application that serves a document root is told the development path once", [] {
                                     using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ClosingFrontend>;
                                     std::ostringstream captured;
                                     struct Redirect {
                                         explicit Redirect(std::ostream& out) {
                                             detail::developmentPathOutput = &out;
                                             detail::developmentPathNoted.clear();
                                         }
                                         ~Redirect() {
                                             detail::developmentPathOutput = nullptr;
                                         }
                                     } const redirect{captured};
                                     const auto notes = [&captured] {
                                         std::size_t count = 0;
                                         const auto  text  = captured.str();
                                         for (auto at = text.find(detail::developmentPathNote); at != std::string::npos;
                                              at      = text.find(detail::developmentPathNote, at + 1))
                                             ++count;
                                         return count;
                                     };
                                     given("an application without a document root", [&] {
                                         FrontendWF webFront;
                                         when("it is shown", [&] {
                                             webFront.show();
                                             then("nothing is said", [&] {
                                                 require(captured.str().empty());
                                             });
                                         });
                                     });
                                     given("two applications that serve a document root", [&] {
                                         FrontendWF first(WebFrontConfig{.documentRoot = "web"});
                                         FrontendWF second(WebFrontConfig{.port = "9003", .documentRoot = "web"});
                                         when("both are shown, one of them twice", [&] {
                                             first.show();
                                             first.show();
                                             second.show();
                                             then("the process names ui/, wf dev and the agent guide exactly once", [&] {
                                                 require(notes() == 1);
                                                 require(captured.str(), contains("TypeScript/TSX interface in ui/"));
                                                 require(captured.str(), contains("`wf dev`"));
                                                 require(captured.str(), contains("docs/DEVELOPING_WITH_AGENTS.md"));
                                             });
                                         });
                                     });
                                 }};

const Scenario showOpens{"show opens the default page and returns when the window session ends", [] {
                             given("a default-constructed application whose frontend completes after open", [&] {
                                 using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, ClosingFrontend>;
                                 FrontendWF webFront;
                                 const int  previousOpenCalls = ClosingFrontend::openCalls;

                                 when("show is called without arguments", [&] {
                                     webFront.show();

                                     then("the default page is opened on the default port", [&] {
                                         require(ClosingFrontend::openCalls == previousOpenCalls + 1);
                                         require(ClosingFrontend::lastPort == "9002");
                                         require(ClosingFrontend::lastFile == "index.html");
                                     });
                                 });

                                 when("show is called with a page", [&] {
                                     webFront.show("greeter.html");

                                     then("that page is opened", [&] {
                                         require(ClosingFrontend::lastFile == "greeter.html");
                                     });
                                 });
                             });
                         }};

const Scenario cppFunctionDeduces{
    "cppFunction deduces a callable's signature from itself",
    [] {
        then("callable_traits resolves the return type and decayed, owning parameter types", [&] {
            auto voidLambda = [](int) {};
            require(std::is_same_v<detail::callable_traits<decltype(voidLambda)>::Return, void>);
            require(std::is_same_v<detail::callable_traits<decltype(voidLambda)>::DecayedArgs, std::tuple<int>>);

            auto valueLambda = [](const std::string& text) {
                return text + "!";
            };
            require(std::is_same_v<detail::callable_traits<decltype(valueLambda)>::Return, std::string>);
            require(std::is_same_v<detail::callable_traits<decltype(valueLambda)>::DecayedArgs, std::tuple<std::string>>);

            auto mutableLambda = [count = 0](int add) mutable {
                return count += add;
            };
            require(std::is_same_v<detail::callable_traits<decltype(mutableLambda)>::Return, int>);
            require(std::is_same_v<detail::callable_traits<decltype(mutableLambda)>::DecayedArgs, std::tuple<int>>);

            require(std::is_same_v<detail::callable_traits<decltype(&freeFunctionForDeductionTest)>::Return, int>);
            require(std::is_same_v<detail::callable_traits<decltype(&freeFunctionForDeductionTest)>::DecayedArgs, std::tuple<double>>);

            std::function<void(std::string)> stdFunction = [](std::string) {};
            require(std::is_same_v<detail::callable_traits<decltype(stdFunction)>::Return, void>);
            require(std::is_same_v<detail::callable_traits<decltype(stdFunction)>::DecayedArgs, std::tuple<std::string>>);
        });
    }};

const Scenario deducedCppFunction{"Deduced cppFunction registration accepts common callable kinds", [] {
                                      given("a WebFront instance", [&] {
                                          using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                                          FrontendWF webFront("9150");

                                          when("callables are registered without explicit template arguments", [&] {
                                              int  observed   = 0;
                                              auto voidLambda = [&observed](int value) {
                                                  observed = value;
                                              };
                                              webFront.cppFunction("onVoid", voidLambda);

                                              auto valueLambda = [](const std::string& text) {
                                                  return text + "!";
                                              };
                                              webFront.cppFunction("onValue", valueLambda);

                                              auto mutableLambda = [count = 0](int add) mutable {
                                                  return count += add;
                                              };
                                              webFront.cppFunction("onMutable", mutableLambda);

                                              webFront.cppFunction("onFreeFunction", &freeFunctionForDeductionTest);

                                              std::function<void(std::string)> stdFunction = [](std::string) {};
                                              webFront.cppFunction("onStdFunction", stdFunction);

                                              then("registration compiles and the original callables remain usable", [&] {
                                                  voidLambda(7);
                                                  require(observed == 7);
                                                  require(valueLambda("hi") == "hi!");
                                              });
                                          });
                                      });
                                  }};

const Scenario cFunction{"C++ function responders encode values and failures", [] {
                             using Policy = http::DefaultBuffersPolicy;
                             using Net    = networking::NetworkingMock;

                             given("a successful result", [&] {
                                 ResultLinkMock link;
                                 auto           responder = detail::makeCppFunctionResponder<Net, Policy, std::string>([](std::span<const std::byte>) {
                                     return std::string{"answer"};
                                 });
                                 responder({}, link, 42);

                                 auto        result  = msg::FunctionReturn<Policy>::castFromRawData(link.message);
                                 auto        payload = result->payload();
                                 std::string value;
                                 result->decodeParameter(value, payload);

                                 then("the correlated value is returned", [&] {
                                     require(result->getCallId() == 42);
                                     require(value == "answer");
                                     require(payload.empty());
                                 });
                             });

                             given("a standard exception", [&] {
                                 ResultLinkMock link;
                                 auto           responder = detail::makeCppFunctionResponder<Net, Policy, void>([](std::span<const std::byte>) {
                                     throw std::runtime_error("callback failed");
                                 });
                                 responder({}, link, 7);

                                 auto        result  = msg::FunctionReturn<Policy>::castFromRawData(link.message);
                                 auto        payload = result->payload();
                                 std::string message;
                                 result->decodeParameter(message, payload);

                                 then("its useful message is encoded", [&] {
                                     require(message == "callback failed");
                                 });
                             });

                             given("a non-standard exception", [&] {
                                 ResultLinkMock link;
                                 auto           responder = detail::makeCppFunctionResponder<Net, Policy, void>([](std::span<const std::byte>) {
                                     throw 42;
                                 });
                                 responder({}, link, 8);

                                 auto        result  = msg::FunctionReturn<Policy>::castFromRawData(link.message);
                                 auto        payload = result->payload();
                                 std::string message;
                                 result->decodeParameter(message, payload);

                                 then("a stable fallback message is encoded", [&] {
                                     require(message == "Unknown C++ exception");
                                 });
                             });

                             given("a fire-and-forget call", [&] {
                                 ResultLinkMock link;
                                 bool           called    = false;
                                 auto           responder = detail::makeCppFunctionResponder<Net, Policy, void>([&called](std::span<const std::byte>) {
                                     called = true;
                                 });
                                 responder({}, link, 0);

                                 then("the callback runs without sending a result", [&] {
                                     require(called);
                                     require(link.message.empty());
                                 });
                             });
                         }};

namespace {

// The single string parameter of the return message a responder sent.
template <http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
std::string sentString(const ResultLinkMock& link) {
    auto        result  = msg::FunctionReturn<Policy>::castFromRawData(link.message);
    auto        payload = result->payload();
    std::string value;
    result->decodeParameter(value, payload);
    if (result->getParametersCount() != 1 || !payload.empty())
        throw std::runtime_error("The return message does not hold exactly one parameter");
    return value;
}

template <typename R, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
std::string respondWith(R value) {
    ResultLinkMock link;
    auto           responder = detail::makeCppFunctionResponder<networking::NetworkingMock, Policy, R>([value](std::span<const std::byte>) {
        return value;
    });
    responder({}, link, 9);
    return sentString<Policy>(link);
}

}  // namespace

namespace {
/// The code and the message of the exception a responder sent.
template <http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
std::pair<std::string, std::string> sentException(std::function<void()> thrower) {
    ResultLinkMock link;
    auto           responder = detail::makeCppFunctionResponder<networking::NetworkingMock, Policy, int>([thrower](std::span<const std::byte>) -> int {
        thrower();
        return 0;
    });
    responder({}, link, 9);
    auto        result  = msg::FunctionReturn<Policy>::castFromRawData(link.message);
    auto        payload = result->payload();
    std::string code, message;
    msg::FunctionReturn<Policy>::decodeException(code, message, payload);
    return {code, message};
}
}  // namespace

const Scenario callbackFailure{"A callback failure is sent with its code", [] {
                                   given("a callback throwing a BridgeError", [&] {
                                       then("the rejection carries its code", [&] {
                                           require(sentException([] {
                                                       throw BridgeError("denied", "no");
                                                   })
                                                   == std::pair<std::string, std::string>{"denied", "no"});
                                       });
                                   });
                                   given("a callback throwing an ordinary exception or something else", [&] {
                                       then("the code is the generic one", [&] {
                                           require(sentException([] {
                                                       throw std::runtime_error("plain");
                                                   })
                                                   == std::pair<std::string, std::string>{std::string(bridge_code::generic), "plain"});
                                           require(sentException([] {
                                                       throw 42;
                                                   }).first
                                                   == bridge_code::generic);
                                       });
                                   });
                                   given("a callback whose message exceeds what a small policy leaves", [&] {
                                       using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 64>;
                                       then("the rejection is still sent, with the message cut to fit", [&] {
                                           const auto [code, message] = sentException<SmallPolicy>([] {
                                               throw BridgeError("quota", std::string(1000, 'x'));
                                           });
                                           require(code == "quota");
                                           require(message == std::string(64 - 8 - 2 - 5 - 2, 'x'));
                                       });
                                       then("a multi-byte character is never cut in two", [&] {
                                           std::string text;
                                           for (int i = 0; i < 500; ++i)
                                               text += "\xc3\xa9";
                                           const auto message = sentException<SmallPolicy>([text] {
                                                                    throw BridgeError("quota", text);
                                                                }).second;
                                           require(message.size() == 46);  // 47 bytes fit, the 24th character would need the 48th
                                           require(message == text.substr(0, 46));
                                       });
                                   });
                                   given("a callback whose argument cannot be decoded", [&] {
                                       ResultLinkMock link;
                                       auto handler = detail::makeCppFunctionHandler<http::DefaultBuffersPolicy, void, std::string>([](std::string) {});
                                       const std::array<std::byte, 1> boolean{std::byte{static_cast<std::uint8_t>(msg::CodedType::booleanTrue)}};
                                       then("the failure has the malformed code", [&] {
                                           try {
                                               handler(boolean);
                                               fail("The call did not throw");
                                           } catch (const BridgeError& error) {
                                               require(error.code() == bridge_code::malformed);
                                           }
                                       });
                                   });
                               }};

const Scenario cString{"C++ string results cross the bridge up to the message size of the policy", [] {
                           const std::string longText(100000, 'x');
                           using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 1024 * 1024>;
                           const std::string overlong(1024 * 1024, 'x');
                           const std::string tooLong = "WebFront message exceeds the maximum message size of the buffers policy";

                           given("a callback returning a string of more than 65535 bytes", [&] {
                               then("it is sent whole as a longString", [&] {
                                   require(respondWith(longText) == longText);
                               });
                           });
                           given("a callback returning a string that does not fit the message size of the policy", [&] {
                               then("a single well-formed exception is sent", [&] {
                                   require(respondWith<std::string, SmallPolicy>(overlong) == tooLong);
                               });
                           });
                           given("a callback returning such a string inside a tuple", [&] {
                               then("the partly encoded result is replaced by a single well-formed exception", [&] {
                                   require(respondWith<std::tuple<int, std::string>, SmallPolicy>(std::tuple<int, std::string>{1, overlong}) == tooLong);
                               });
                           });
                       }};

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

const Scenario cCallback{"A C++ callback can receive the browser that called it", [] {
                             using Policy = http::DefaultBuffersPolicy;

                             given("a handler whose callable takes the caller before its JavaScript parameters", [&] {
                                 auto handler = detail::makeCppFunctionHandler<Policy, std::string, std::string>(
                                     [](const CallerMock& caller, const std::string& text) {
                                         return std::to_string(caller.id) + ":" + text;
                                     });
                                 const auto data = smallStringData("hi");

                                 then("the caller is passed first and only the remaining parameters are decoded", [&] {
                                     require(handler(data, CallerMock{3}) == "3:hi");
                                 });
                             });
                             given("a callable taking the caller by mutable reference, adapted to its declared type", [&] {
                                 auto handler = detail::makeCppFunctionHandler<Policy, std::string, std::string>(
                                     detail::adaptCaller<CallerMock&>([](CallerMock& caller, const std::string& text) {
                                         caller.id += 1;
                                         return std::to_string(caller.id) + ":" + text;
                                     }));
                                 CallerMock caller{3};
                                 const auto data = smallStringData("hi");

                                 then("it receives the caller itself", [&] {
                                     require(handler(data, caller) == "4:hi");
                                     require(caller.id == 4);
                                 });
                             });
                             given("a responder for such a handler", [&] {
                                 ResultLinkMock link;
                                 auto           handler = detail::makeCppFunctionHandler<Policy, std::string>([](CallerMock caller) {
                                     return std::to_string(caller.id);
                                 });
                                 auto responder         = detail::makeCppFunctionResponder<networking::NetworkingMock, Policy, std::string>(std::move(handler));
                                 responder({}, link, 5, CallerMock{11});

                                 then("the caller reaches the callable and the result is returned", [&] {
                                     require(sentString(link) == "11");
                                 });
                             });
                         }};

// Only a first parameter of the application's UI type, with any reference or const, is the caller.
using CallerTestUI = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>::UI;
static_assert(detail::takes_caller<CallerTestUI, CallerTestUI, std::string>::value);
static_assert(detail::takes_caller<CallerTestUI, const CallerTestUI&>::value);
static_assert(!detail::takes_caller<CallerTestUI, std::string, CallerTestUI>::value);
static_assert(!detail::takes_caller<CallerTestUI>::value);

const Scenario uIFirst{"A UI first parameter selects the caller form of cppFunction", [] {
                           using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                           using AppUI      = FrontendWF::UI;

                           given("an application", [&] {
                               FrontendWF webFront("9161");
                               then("the caller may be taken by value or by const reference", [&] {
                                   requireNothrow([&] {
                                       webFront.cppFunction("byValue", [](AppUI) {});
                                   });
                                   requireNothrow([&] {
                                       webFront.cppFunction("byConstReference", [](const AppUI&) {});
                                   });
                               });
                               then("the caller may be taken by mutable or rvalue reference", [&] {
                                   requireNothrow([&] {
                                       webFront.cppFunction("byReference", [](AppUI&) {});
                                   });
                                   requireNothrow([&] {
                                       webFront.cppFunction("byRvalueReference", [](AppUI&&) {});
                                   });
                               });
                               then("caller callbacks register through deduction and explicit types", [&] {
                                   requireNothrow([&] {
                                       webFront.cppFunction("deduced", [](AppUI, std::string) {});
                                   });
                                   requireNothrow([&] {
                                       webFront.template cppFunction<int, const AppUI&, double>("explicit", [](const AppUI&, double) {
                                           return 1;
                                       });
                                   });
                               });
                               then("browser subscriptions can be taken, and released after the application", [&] {
                                   UISubscription subscription;
                                   {
                                       FrontendWF scoped("9162");
                                       subscription = scoped.subscribeUIStarted([](AppUI) {});
                                       require(subscription.active());
                                   }
                                   requireNothrow([&] {
                                       subscription.reset();
                                   });
                               });
                           });
                       }};

const Scenario webFrontConfigHas{"WebFrontConfig has newcomer-friendly defaults", [] {
                                     then("the defaults are loopback binding, an empty port (9002, else a free one), and no local document root", [&] {
                                         WebFrontConfig config;
                                         require(config.address == "127.0.0.1");
                                         require(config.port.empty());
                                         require(config.documentRoot.empty());
                                     });
                                 }};

const Scenario basicWFSupports{"BasicWF supports default construction", [] {
                                   given("a frontend and networking mock", [&] {
                                       when("a BasicWF is default-constructed", [&] {
                                           using DefaultCtorWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                                           then("construction succeeds without specifying port, root, page, frontend, or callbacks", [&] {
                                               requireNothrow([&] {
                                                   DefaultCtorWF{};
                                               });
                                           });
                                       });
                                   });
                               }};

const Scenario basicWFDoes{"BasicWF does not require onUIStarted to be set", [] {
                               // Note: this exercises construction/registration/teardown without onUIStarted. The guarded
                               // dispatch itself - onEvent's `if (uiStartedHandler) uiStartedHandler(...)` for the `linked`
                               // event - would need a full HTTP-upgrade-then-WebSocket-handshake simulation through the mock
                               // networking stack to exercise end-to-end; that infrastructure doesn't exist yet in this test
                               // suite, so this guard is otherwise verified by inspection (a plain std::function truthiness
                               // check around the call).
                               given("a BasicWF that never registers onUIStarted", [&] {
                                   using FrontendWF = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                                   when("it is constructed and used", [&] {
                                       then("nothing throws", [&] {
                                           requireNothrow([&] {
                                               [] {
                                                   FrontendWF webFront("9160");
                                                   webFront.cppFunction<void>("noop", [] {});
                                               }();
                                           });
                                       });
                                   });
                               });
                           }};

namespace {
// Preserve the legacy empty-options call despite the new ContentPolicy overload.
static_assert(requires { frontend::WebView2Frontend::open("127.0.0.1", "9002", "index.html", {}); });

struct PolicyFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static inline ContentPolicy       received;
    static inline std::string         receivedAddress;
    static void                       initialize() {}
    static void                       open(std::string_view, std::string_view) {
        throw std::runtime_error("Legacy overload selected");
    }
    static void open(std::string_view address, std::string_view, std::string_view, const ContentPolicy& policy) {
        receivedAddress = address;
        received        = policy;
    }
};
}  // namespace

const Scenario webFrontPasses{
    "WebFront passes per-application content rules to policy-aware frontends",
    [] {
        using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, PolicyFrontend>;
        App app(WebFrontConfig{
            .address       = "127.0.0.2",
            .contentPolicy = {.navigationAllowedOrigins = {"https://docs.example"}, .popups = PopupPolicy::systemBrowser, .devToolsEnabled = false}
        });
        requireNothrow([&] {
            app.openWindow("index.html");
        });
        then("the configured address reaches the preferred overload", [&] {
            require(PolicyFrontend::receivedAddress == "127.0.0.2");
        });
        then("the navigation allow-list is preserved", [&] {
            require(PolicyFrontend::received.navigationAllowedOrigins == std::vector<std::string>{"https://docs.example"});
        });
        then("popup and DevTools settings are preserved independently", [&] {
            require(PolicyFrontend::received.popups == PopupPolicy::systemBrowser);
            require(!PolicyFrontend::received.devToolsEnabled);
        });
    }};

const Scenario ephemeralPorts{"Ephemeral ports are bound atomically and forwarded to the frontend", [] {
                                  using App = BasicWFWithFrontend<NetProvider, TestFilesystem, AddressFrontend>;
                                  App first{WebFrontConfig{.port = "0"}};
                                  App second{WebFrontConfig{.port = "0"}};
                                  first.openWindow("index.html");
                                  const auto firstPort = AddressFrontend::lastPort;
                                  second.openWindow("index.html");
                                  require(firstPort != "0");
                                  require(AddressFrontend::lastPort != "0");
                                  require(firstPort != AddressFrontend::lastPort);
                                  require(std::stoi(firstPort) > 0);
                                  require(std::stoi(AddressFrontend::lastPort) > 0);
                                  require(first.port() == firstPort);
                                  require(second.port() == AddressFrontend::lastPort);
                                  given("a legacy provider without a bound-endpoint query", [&] {
                                      using MockApp = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, AddressFrontend>;
                                      MockApp mock{WebFrontConfig{.port = "0"}};
                                      require(mock.port() == "0");
                                  });
                              }};

const Scenario sendingDOM{"Sending a DOM batch through a lost UI link throws ConnectionError", [] {
                              using App = BasicWFWithFrontend<WebFrontNetworkingMock, TestFilesystem, InitializingFrontend>;
                              given("a UI handle whose connection is absent from the application", [&] {
                                  App           app("9161");
                                  App::UI       ui(app, 42, 1);
                                  msg::DomBatch batch(7);
                                  batch.add({msg::DomOp::text, 1, 0, 0, "unreachable"});
                                  then("sending fails synchronously without queuing a frame", [&] {
                                      requireThrows<ConnectionError>([&] {
                                          ui.sendDomBatch(batch);
                                      });
                                      require(ui.pendingWrites() == 0);
                                  });
                              });
                          }};

const Scenario uIAssignable{"A UI is an assignable value identifying one browser of one application", [] {
                                using App = BasicWF<networking::NetworkingMock, fs::DefaultFS>;
                                given("an application over the networking mock", [&] {
                                    App app(WebFrontConfig{.port = "0"});
                                    then("a UI is an assignable value usable in containers", [&] {
                                        static_assert(std::is_copy_assignable_v<App::UI>);
                                        static_assert(std::is_nothrow_move_assignable_v<App::UI>);
                                        static_assert(std::equality_comparable<App::UI>);
                                        static_assert(!std::is_copy_constructible_v<App> && !std::is_move_constructible_v<App>);
                                        static_assert(!std::is_copy_assignable_v<App> && !std::is_move_assignable_v<App>);
                                        std::vector<App::UI> clients{App::UI(app, 1, 1), App::UI(app, 2, 1), App::UI(app, 3, 1)};
                                        std::erase_if(clients, [](const App::UI& ui) {
                                            return ui.id() == 2;
                                        });
                                        require(clients.size() == 2);
                                        check(clients[0] == App::UI(app, 1, 1));
                                        check(clients[0] != clients[1]);
                                        check(clients[1].id() == 3);
                                        std::optional<App::UI> current;
                                        current = clients[1];
                                        check(current->id() == 3);
                                    });
                                    then("a UI of another application is never equal, even with the same identifier", [&] {
                                        struct Application {
                                        } first, second;
                                        using StubUI = BasicUI<Application>;
                                        check(StubUI(first, 1, 1) != StubUI(second, 1, 1));
                                        check(StubUI(first, 1, 1) != StubUI(first, 1, 2));  // same identifier, later connection
                                        StubUI ui(first, 1, 1);
                                        ui = StubUI(second, 1, 1);
                                        check(ui == StubUI(second, 1, 1));
                                    });
                                    then("a UI has no default state and a reassigned one acts on its new value", [&] {
                                        static_assert(!std::is_default_constructible_v<App::UI>);
                                        App::UI       ui(app, 1, 1);
                                        const App::UI gone(app, 42, 1);
                                        ui = gone;
                                        check(ui == gone);
                                        checkThrows<ConnectionError>([&] {
                                            ui.addScript("1");
                                        });
                                        check(ui.pendingWrites() == 0);
                                        checkNothrow([&] {
                                            ui.jsFunction("console.log")("dropped");
                                        });
                                        auto result = ui.jsFunction<int>("Math.abs")(-1);
                                        checkThrows(
                                            [&] {
                                                (void)result.get();
                                            },
                                            equals("Browser connection closed"));
                                        checkNothrow([&] {
                                            (void)ui.writesAtMost(0);
                                        });
                                        App::UI moved(std::move(ui));
                                        check(moved.id() == 42);
                                        checkThrows<ConnectionError>([&] {
                                            moved.addScript("1");
                                        });
                                    });
                                });
                            }};

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

const Scenario dOMReadiness{"DOM readiness explains a missing network timer while the ordinary bridge remains usable", {"dom"}, [] {
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
                                Socket::receive(
                                    testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
                                require(app.connectedBrowsers() == 1);
                                auto call = [](std::string name, msg::CallId id) {
                                    msg::FunctionCall<> message;
                                    message.setCallId(id);
                                    websocket::Frame<DomNoTimerNet> frame{
                                        std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
                                    message.encodeParameter(name, frame);
                                    frame.freeze();
                                    Socket::receive(testing::clientFrame(testing::messagePayload(frame)));
                                };
                                call("wfDom.ready", 21);
                                require(Socket::wrote("DOM API requires Net::ResumeAfter"));
                                call("echo", 22);
                                require(Socket::wrote("bridge stays available"));
                                require(app.connectedBrowsers() == 1);
                                app.shutdown();
                            }};
