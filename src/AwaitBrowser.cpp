#include <WebFront.hpp>

// LCOV_EXCL_START - interactive example; the shared Jasmine suite exercises this sender API.
webfront::Task showSquareRoot(webfront::UI ui) {
    auto result = co_await ui.jsFunctionSender<double>("Math.sqrt")(81);
    co_await ui.jsFunctionSender("console.log")("sqrt(81) =", result);
}

int main() {
    webfront::WebFront app;
    app.onUIStarted([&app](webfront::UI ui) {
        app.spawn(showSquareRoot(ui));
    });
    app.show();
}
// LCOV_EXCL_STOP
