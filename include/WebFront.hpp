/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include "JsFunction.hpp"
#include "http/HTTPServer.hpp"
#include "tooling/HexDump.hpp"
#include "utils/TypeErasedFunction.hpp"
#include "system/IndexFS.hpp"
#include "weblink/WebLink.hpp"

#include "networking/TCPNetworkingTS.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace webfront {
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

template<typename NetProvider, typename Filesystem>
class BasicWF {
public:
    using Net = NetProvider;
    using UI = BasicUI<BasicWF<Net, Filesystem>>;

    BasicWF(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot), idsCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket)
                for (bool inserted = false; !inserted; ++idsCounter)
                    std::tie(std::ignore, inserted) = webLinks.try_emplace(idsCounter, std::move(socket), idsCounter,
                                                                           [this](WebLinkEvent event) { onEvent(event); });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }

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
            auto deserializeAndCall = [&]<std::size_t... Is>(std::tuple<Args...> & tuple, std::index_sequence<Is...>) {
                (msg::FunctionCall::decodeParameter(std::get<Is>(tuple), data), ...);
                function(std::get<Is>(tuple)...);
            };

            deserializeAndCall(parameters, std::index_sequence_for<Args...>());
        });
    }

private:
    http::Server<Net, Filesystem> httpServer;
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
