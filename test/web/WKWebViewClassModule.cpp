// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-19
//
// WebFront — tests/web
// Role: Translation unit that compiles WKWebView.hpp in a module-style build.
// Thread: ui-loop | Alloc: zero-hot-path
// LLM: macOS only; compile check

#include <webfront/frontend/WKWebView.hpp>

using namespace webfront::frontend::detail::wkwebview;

extern "C" __attribute__((visibility("default"))) void* webfrontWKDelegateClass() {
    return delegateClass();
}

extern "C" __attribute__((visibility("default"))) void* webfrontWKViewClass() {
    return viewClass();
}

extern "C" __attribute__((visibility("default"))) void webfrontWKModuleSession() {
    Object  pool(send(type("NSAutoreleasePool"), "new"));
    Session session("http://127.0.0.1:0/", {.visible = false});
    session.create();
    send<void>(session.window, "performClose:", static_cast<id>(nullptr));
    if (!session.closed)
        throw std::runtime_error("A delegate registered by another image did not close the module session");
    session.run();
}
