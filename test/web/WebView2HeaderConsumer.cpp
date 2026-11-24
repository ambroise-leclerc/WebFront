// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-16
//
// WebFront — tests/web
// Role: Translation unit that includes WebView2.hpp to check the header is self-contained.
// Thread: ui-loop | Alloc: zero-hot-path
// LLM: Windows only; compile check

// A second translation unit catches standalone include and ODR regressions.

#include <webfront/frontend/WebView2.hpp>

bool webView2ClosesServer() {
    static_assert(webfront::frontend::WebView2Frontend::action == webfront::frontend::Action::closeServerAfterOpen);
    return true;
}
