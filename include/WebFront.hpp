/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once

#include "frontend/Frontend.hpp"
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
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

namespace detail {
template <frontend::FrontendType Frontend>
inline int ensureFrontendInitialized() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        Frontend::initialize();
    });
    return 0;
}
}  // namespace detail

template <typename NetProvider, typename Filesystem, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy,
          frontend::FrontendType Frontend = frontend::DefaultFrontend>
class BasicWF {
public:
    using Net              = NetProvider;
    using BufferPolicy     = Policy;
    using FrontendProvider = Frontend;
    using UI               = BasicUI<BasicWF<Net, Filesystem, Policy, Frontend>>;

    explicit BasicWF(std::string_view port, std::filesystem::path docRoot = ".")
        : httpServer((detail::ensureFrontendInitialized<Frontend>(), "0.0.0.0"), port, docRoot), httpPort(port), httpDocRoot(docRoot), idsCounter(0) {
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
                (msg::FunctionCall<Policy>::decodeParameter(std::get<Is>(tuple), data), ...);
                function(std::get<Is>(tuple)...);
            };

            deserializeAndCall(parameters, std::index_sequence_for<Args...>());
        });
    }

    enum class WindowAction { none, closeWindow };
    WindowAction openWindow(std::string_view htmlFilename) {
        Frontend::open(httpPort, htmlFilename);
        if constexpr (Frontend::action == frontend::Action::closeServerAfterOpen)
            return WindowAction::closeWindow;
        else
            return WindowAction::none;
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
    http::Server<Net, Filesystem, Policy>                                    httpServer;
    std::string_view                                                         httpPort;
    std::filesystem::path                                                    httpDocRoot;
    std::map<WebLinkId, WebLink<Net, Policy>>                                webLinks;
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

template <typename NetProvider, typename Filesystem, frontend::FrontendType Frontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using BasicWFWithFrontend = BasicWF<NetProvider, Filesystem, Policy, Frontend>;

using WebFront = BasicWF<NetProvider, fs::IndexFS>;
template <frontend::FrontendType Frontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using WebFrontWithFrontend = BasicWF<NetProvider, fs::IndexFS, Policy, Frontend>;
using UI       = WebFront::UI;

}  // namespace webfront
