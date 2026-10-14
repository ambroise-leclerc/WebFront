#include <WebFront.hpp>

#include <string>

// Switching to an embedded Chromium window only changes this alias (CEF must be enabled at
// configure time with -DWEBFRONT_EMBED_CEF=ON):
//   using App = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
using App = webfront::WebFront;

// LCOV_EXCL_START - interactive example; the greeting callback is covered by the unit tests.
int main() {
    // Development-only: serves the files of this directory (relative to the working directory,
    // i.e. run from the build directory) without any sandboxing. Not for production use.
    App app{webfront::WebFrontConfig{.documentRoot = "src/greeter"}};

    app.cppFunction("greet", [](std::string name) {
        return "Hello, " + name + "!";
    });

    app.show();
}
// LCOV_EXCL_STOP
