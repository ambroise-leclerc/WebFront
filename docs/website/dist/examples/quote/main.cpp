#include <webfront/WebFront.hpp>
#include <webfront/testing/Jasmine.hpp>
#include <stdexcept>

using App = webfront::WebFrontWithFrontend<webfront::frontend::WKWebViewFrontend>;

int main() {
    App app{{.port = "0", .documentRoot = "ui"}};
    app.cppFunction("quote.calculate", [](int count) {
        if (count < 1 || count > 1000)
            throw std::invalid_argument("Invalid quantity");
        return count * 25;
    });
    webfront::testing::JasmineTest tests{app};
    app.show();
    app.shutdown();
    return tests.exitCode();
}
