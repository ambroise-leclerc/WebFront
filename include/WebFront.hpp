/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include "JsFunction.hpp"
#include "WebLink.hpp"
#include "http/HTTPServer.hpp"
#include "tooling/HexDump.hpp"
#include "utils/TypeErasedFunction.hpp"

#include "networking/TCPNetworkingTS.hpp"

#include <map>
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
            webFront.getLink(webLinkId).sendCommand(msg::TextCommand(TxtOpcode::injectScript, script));
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
    [[nodiscard]] JsFunction<WebFront> jsFunction(std::string_view functionName) const { return JsFunction{functionName, webFront, webLinkId}; }
};

template<typename NetProvider>
class BasicWF {
public:
    using Net = NetProvider;
    using UI = BasicUI<BasicWF<Net>>;

    BasicWF(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot), idsCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket)
                for (bool inserted = false; !inserted; ++idsCounter)
                    std::tie(std::ignore, inserted) =
                      webLinks.try_emplace(idsCounter, std::move(socket), idsCounter, [this](WebLinkEvent event) { onEvent(event); });
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
        cppFunctions.try_emplace(functionName, [&function, this](WebLinkId id) -> R {
            std::tuple<Args...> parameters;
            auto& webLink = getLink(id);
            auto deserializeAndCall = [ this, &webLink, &function ]<std::size_t... Is>(std::tuple<Args...> & tuple, std::index_sequence<Is...>) {
                (webLink.extractNext(std::get<Is>(tuple)), ...);
                function(std::get<Is>(tuple)...);
            };

            deserializeAndCall(parameters, std::index_sequence_for<Args...>());
            if constexpr (!std::is_same_v<R, void>) return {};
        });
    }

    void deserialize(size_t index, auto&& param) {
        std::cout << "Param " << index << " : ";
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(param)>, std::string>) {
            std::cout << "string ";
            param = std::string("s");
        }

        else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(param)>, int>) {
            std::cout << "int ";
            param = 13;
        }

        else
            static_assert(false, "This type conversion from Javascript type is not handled");
    }

private:
    http::Server<Net> httpServer;
    std::map<WebLinkId, WebLink<Net>> webLinks;
    WebLinkId idsCounter;
    std::function<void(UI)> uiStartedHandler;
    std::map<std::string, utils::TypeErasedFunction> cppFunctions;

private:
    void onEvent(WebLinkEvent event) {
        switch (event.code) {
        case WebLinkEvent::Code::linked: uiStartedHandler(UI{*this, event.webLinkId}); break;
        case WebLinkEvent::Code::closed: webLinks.erase(event.webLinkId); break;
        case WebLinkEvent::Code::cppFunctionCalled: 
            cppFunctions.at(event.text)(event.webLinkId); break;
        }
    }
};

using WebFront = BasicWF<NetProvider>;
using UI = WebFront::UI;

} // namespace webfront
