// A second translation unit catches standalone include and ODR regressions.
#include <frontend/WebView2.hpp>

bool webView2ClosesServer() {
    static_assert(webfront::frontend::WebView2Frontend::action == webfront::frontend::Action::closeServerAfterOpen);
    return true;
}
