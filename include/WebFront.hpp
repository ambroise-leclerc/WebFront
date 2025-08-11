/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once

// Prevent Windows min/max macros from interfering with std::min/max
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Ensure WinSock2.h is included before any headers that might include WinSock.h
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "JsFunction.hpp"
#include "frontend/CEF.hpp"
#include "frontend/DefaultBrowser.hpp"
#include "http/HTTPServer.hpp"
#include "system/IndexFS.hpp"
#include "tooling/HexDump.hpp"
#include "utils/TypeErasedFunction.hpp"
#include "weblink/WebLink.hpp"

#include "networking/TCPNetworkingTS.hpp"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace webfront {

inline constexpr std::string_view version = "0.1.0";

using NetProvider = networking::TCPNetworkingTS;
using ConnectionError = std::runtime_error;

template<typename WebFront>
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
        }
        catch (const std::out_of_range&) {
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
} // namespace detail

template<typename NetProvider, typename Filesystem>
class BasicWF {
public:
    using Net = NetProvider;
    using UI = BasicUI<BasicWF<Net, Filesystem>>;

    BasicWF(std::string_view port, std::filesystem::path docRoot = ".")
        : httpServer((detail::ensureCEFInitialized(), "0.0.0.0"), port, docRoot), httpPort(port), httpDocRoot(docRoot), idsCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket)
                for (bool inserted = false; !inserted; ++idsCounter)
                    std::tie(std::ignore, inserted) = webLinks.try_emplace(idsCounter, std::move(socket), idsCounter,
                                                                           [this](WebLinkEvent event) { onEvent(event); });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }

    void stop() { httpServer.stop(); }

    void onUIStarted(std::function<void(UI)>&& handler) { uiStartedHandler = std::move(handler); }
    WebLink<Net>& getLink(WebLinkId id) { return webLinks.at(id); }

    /**
     * @brief Registers a function which will be callable from Javascript.
     *
     * @tparam R ReturnType of the CppFunction
     * @tparam Args parameters of the CppFunction
     * @param functionName
     * @param function
     */
    template<typename R, typename... Args>
    void cppFunction(std::string functionName, auto&& function) {
        cppFunctions.try_emplace(functionName, [&function](std::span<const std::byte> data) -> void {
            std::tuple<Args...> parameters;
            auto deserializeAndCall = [&]<std::size_t... Is>(std::tuple<Args...>& tuple, std::index_sequence<Is...>) {
                (msg::FunctionCall::decodeParameter(std::get<Is>(tuple), data), ...);
                function(std::get<Is>(tuple)...);
            };

            deserializeAndCall(parameters, std::index_sequence_for<Args...>());
        });
    }

    void openWindow(std::string_view htmlFilename) {
        if constexpr (cef::webfrontEmbedCEF)
            cef::open(httpPort, htmlFilename);
        else
            browser::open(httpPort, htmlFilename);
    }

private:
    http::Server<Net, Filesystem> httpServer;
    std::string_view httpPort;
    std::filesystem::path httpDocRoot;
    std::map<WebLinkId, WebLink<Net>> webLinks;
    WebLinkId idsCounter;
    std::function<void(UI)> uiStartedHandler;
    std::map<std::string, std::function<void(std::span<const std::byte>)>> cppFunctions;

private:
    void onEvent(WebLinkEvent event) {
        switch (event.code) {
        case WebLinkEvent::Code::linked: uiStartedHandler(UI{*this, event.webLinkId}); break;
        case WebLinkEvent::Code::closed: webLinks.erase(event.webLinkId); break;
        case WebLinkEvent::Code::cppFunctionCalled: cppFunctions.at(event.text)(event.data); break;
        }
    }
};

using WebFront = BasicWF<NetProvider, fs::IndexFS>;
using UI = WebFront::UI;

} // namespace webfront
