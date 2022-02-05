#include <catch2/catch.hpp>

#include <JsFunction.hpp>

using namespace webfront;
using namespace std;

namespace webfront::websocket {
class FrameWithBuffers : Frame {};
} // namespace webfront::websocket

struct WebLinkMock {
    WebLinkMock(WebLinkId id) : webLinkId(id) {}
    void sendCommand(websocket::FrameWithBuffers& ) {}

    WebLinkId webLinkId;
};

struct WebFrontMock {
    // webFront.getLink(webLinkId).sendCommand(paramSpans);
    WebLinkMock getLink(WebLinkId id) { return WebLinkMock{id}; }
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
