#include <catch2/catch.hpp>

#include <JsFunction.hpp>
#include <networking/NetworkingMock.hpp>

using namespace webfront;
using namespace std;

template<typename WebFront>
struct WebLinkMock {
    WebLinkMock(WebLinkId id, WebFront& wf) : webLinkId(id), webFront(wf) {}
    //void sendCommand(websocket::Frame& ) {}

    WebLinkId webLinkId;
    WebFront& webFront;
    void sendFrame(websocket::Frame<typename WebFront::Net>) {}
};

struct WebFrontMock {
    using Net = networking::NetworkingMock;
    // webFront.getLink(webLinkId).sendCommand(paramSpans);
    WebLinkMock<WebFrontMock> getLink(WebLinkId id) { return WebLinkMock<WebFrontMock>{id, *this}; }
};

SCENARIO("JsFunction") {
    WebFrontMock webFront;
    WebLinkId id{1};

    JsFunction print("print", webFront, id);
    std::string text{"maFunction"};
    const char* text2 = "Text2";

    // int data[] = { 1, 2, 3};

    print(true, "Texte", 45, text, text2 /*, data, &data[0]*/);
}
