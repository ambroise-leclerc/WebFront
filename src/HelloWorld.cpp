#include <WebFront.hpp>

// Switching to an embedded Chromium window only changes this alias (CEF must be enabled at
// configure time with -DWEBFRONT_EMBED_CEF=ON):
//   using App = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
using App = webfront::WebFront;

// LCOV_EXCL_START - interactive example; its page is served and verified by the unit tests.
int main() {
    App app;
    app.show();
}
// LCOV_EXCL_STOP
