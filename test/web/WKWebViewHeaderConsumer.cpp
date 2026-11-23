// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-19
//
// WebFront — tests/web
// Role: Translation unit that includes WKWebView.hpp to check the header is self-contained.
// Thread: ui-loop | Alloc: zero-hot-path
// LLM: macOS only; compile check

#include <webfront/frontend/WKWebView.hpp>

static_assert(requires {
    webfront::frontend::WKWebViewFrontend::open("127.0.0.1", "9002", "index.html", {});
    webfront::frontend::WKWebViewFrontend::open("127.0.0.1", "9002", "index.html", webfront::frontend::WKWebViewOptions{});
});

// A second plain C++ translation unit exercises header-only ODR safety and transitive flags.
void checkWKWebViewHeader() {
    static_assert(webfront::frontend::WKWebViewFrontend::action == webfront::frontend::Action::closeServerAfterOpen);
    webfront::frontend::WKWebViewFrontend::initialize();
}
