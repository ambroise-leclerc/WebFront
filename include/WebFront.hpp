/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once

#include "frontend/CEF.hpp"
#include "frontend/DefaultBrowser.hpp"
#include "http/HTTPServer.hpp"
#include "JsFunction.hpp"
#include "networking/TCPNetworkingTS.hpp"
#include "system/IndexFS.hpp"
#include "system/WindowsCompat.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <stdexcept>
#include <thread>
#include <utility>

namespace webfront {

inline constexpr std::string_view version = "0.1.0";

using NetProvider     = networking::TCPNetworkingTS;
using ConnectionError = std::runtime_error;

template <typename WebFront>
class BasicUI {
    WebFront& webFront;
    WebLinkId webLinkId;

public:
    BasicUI(WebFront& wf, WebLinkId id) : webFront(wf), webLinkId(id) {}

    /**
     * @brief Injects a Javascript script in the client
     *
     * @param script
     */
    void addScript(std::string_view script) const {
        try {
            webFront.getLink(webLinkId).sendCommand(msg::TextCommand(msg::TxtOpcode::injectScript, script));
        } catch (const std::out_of_range&) {
            throw ConnectionError("Connection with client lost");
        }
    }

    /**
     * @brief Creates a Javascript function object.
     *
     * @param functionName
     * @return JsFunction<WebFront>
     */
    [[nodiscard]] JsFunction<WebFront> jsFunction(std::string_view functionName) const {
        return JsFunction{functionName, webFront, webLinkId};
    }
};

// Global initialization helper to ensure CEF initializes before any networking
namespace detail {
inline int ensureCEFInitialized() {
    if constexpr (cef::webfrontEmbedCEF) {
        static bool initialized = false;
        if (!initialized) {
            try {
                cef::initialize();
                initialized = true;
            } catch (const cef::CEFSubprocessExit& e) {
                // If this is a CEF subprocess, exit immediately
                std::exit(e.exit_code());
            } catch (const cef::CEFInitializationError& e) {
                // CEF initialization failed - this is a fatal error
                throw std::runtime_error(e.what());
            }
        }
    }
    return 0;  // Return value for comma operator
}
}  // namespace detail

template <typename NetProvider, typename Filesystem>
class BasicWF {
public:
    using Net = NetProvider;
    using UI  = BasicUI<BasicWF<Net, Filesystem>>;

    explicit BasicWF(std::string_view port, std::filesystem::path docRoot = ".")
        : httpServer((detail::ensureCEFInitialized(), "0.0.0.0"), port, docRoot), httpPort(port), httpDocRoot(docRoot), idsCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket)
                for (bool inserted = false; !inserted; ++idsCounter)
                    std::tie(std::ignore, inserted) = webLinks.try_emplace(idsCounter, std::move(socket), idsCounter, [this](WebLinkEvent event) {
                        onEvent(event);
                    });
        });
    }

    ~BasicWF() {
        // Ensure clean shutdown if user forgot to stop explicitly
        stop();
        if (serverThread.joinable())
            serverThread.join();
    }

    void run() {
        httpServer.run();
    }
    void runOne() {
        httpServer.runOne();
    }

    void stop() {
        httpServer.stop();
    }

    void onUIStarted(std::function<void(UI)>&& handler) {
        uiStartedHandler = std::move(handler);
    }
    WebLink<Net>& getLink(WebLinkId id) {
        return webLinks.at(id);
    }

    /**
     * @brief Registers a function which will be callable from Javascript.
     *
     * @tparam R ReturnType of the CppFunction
     * @tparam Args parameters of the CppFunction
     * @param functionName
     * @param function
     */
    template <typename R, typename... Args>
    void cppFunction(std::string functionName, auto&& function) {
        cppFunctions.try_emplace(functionName, [&function](std::span<const std::byte> data) -> void {
            std::tuple<Args...> parameters;
            auto                deserializeAndCall = [&]<std::size_t... Is>(std::tuple<Args...>& tuple, std::index_sequence<Is...>) {
                (msg::FunctionCall::decodeParameter(std::get<Is>(tuple), data), ...);
                function(std::get<Is>(tuple)...);
            };

            deserializeAndCall(parameters, std::index_sequence_for<Args...>());
        });
    }

    enum class WindowAction { none, closeWindow };
    WindowAction openWindow(std::string_view htmlFilename) {
        if constexpr (cef::webfrontEmbedCEF) {
            cef::open(httpPort, htmlFilename);  // Blocking until window closed
            return WindowAction::closeWindow;
        } else {
            browser::open(httpPort, htmlFilename);  // Non-blocking external browser
            return WindowAction::none;
        };
    }

    // Starts the HTTP server in a background thread, opens the window (blocking for embedded CEF),
    // then stops the server when the window closes (CEF case). For external browser we keep it running.
    void openAndRun(std::string_view htmlFilename) {
        // Launch server only once or restart if previous thread ended
        if (!serverThread.joinable()) {
            serverThread = std::thread([this] {
                run();
            });
        }
        auto action = openWindow(htmlFilename);
        if (action == WindowAction::closeWindow) {
            stop();
            if (serverThread.joinable())
                serverThread.join();
        }
    }

private:
    http::Server<Net, Filesystem>                                          httpServer;
    std::string_view                                                       httpPort;
    std::filesystem::path                                                  httpDocRoot;
    std::map<WebLinkId, WebLink<Net>>                                      webLinks;
    WebLinkId                                                              idsCounter{0};
    std::function<void(UI)>                                                uiStartedHandler;
    std::map<std::string, std::function<void(std::span<const std::byte>)>> cppFunctions;
    std::thread                                                            serverThread;  // Background thread running the HTTP server

private:
    void onEvent(WebLinkEvent event) {
        switch (event.code) {
            case WebLinkEvent::Code::linked:
                uiStartedHandler(UI{*this, event.webLinkId});
                break;
            case WebLinkEvent::Code::closed:
                webLinks.erase(event.webLinkId);
                break;
            case WebLinkEvent::Code::cppFunctionCalled:
                cppFunctions.at(event.text)(event.data);
                break;
        }
    }
};

using WebFront = BasicWF<NetProvider, fs::IndexFS>;
using UI       = WebFront::UI;

}  // namespace webfront
