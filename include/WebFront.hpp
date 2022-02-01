/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include "WebLink.hpp"
#include "tooling/HexDump.hpp"
#include "http/HTTPServer.hpp"

#include "networking/TCPNetworkingTS.hpp"

#include <map>
#include <string_view>
#include <tuple>

namespace webfront {

using NetProvider = networking::TCPNetworkingTS;

template<typename WebFrontType>
class BasicUI {
    WebFrontType& webFront;
    WebLinkId webLinkId;
public:
    BasicUI(WebFrontType& wf, WebLinkId id) : webFront(wf), webLinkId(id) {}
};

template<typename Net>
class BasicWF {
public:
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

private:
    http::Server<Net> httpServer;
    std::map<WebLinkId, WebLink<Net>> webLinks;
    WebLinkId idsCounter;
    std::function<void(UI)> uiStartedHandler;

private:
    void onEvent(WebLinkEvent event) {
        log::debug("onEvent");
        switch (event.code) {
        case WebLinkEvent::Code::linked: uiStartedHandler(UI{*this, event.webLinkId}); break;
        case WebLinkEvent::Code::closed: webLinks.erase(event.webLinkId); break;
        }
    }
};

using WebFront = BasicWF<NetProvider>;
using UI = WebFront::UI;
} // namespace webfront