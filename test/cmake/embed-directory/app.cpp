// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-26
//
// WebFront — tests/cmake
// Role: The application of docs/EMBEDDED_ASSETS.md ("Embedding an interface directory from CMake"), compiled, not run.
// Thread: ui-loop | Alloc: unrestricted
// LLM: keep it identical to the guide's snippet; it proves the snippet compiles with a generated provider

#include <AppUI.hpp>  // generated in the build tree, on the include path of `app`
#include <webfront/WebFront.hpp>

#include <string_view>

using App = webfront::BasicWF<webfront::NetProvider, webfront::fs::DocumentRootFirstFSWith<app::AppUI>>;

int main(int argc, char** argv) {
    webfront::WebFrontConfig config;
    if (argc == 3 && std::string_view{argv[1]} == "--ui")
        config.documentRoot = argv[2];  // development: the files on disk are served in front of the embedded ones
    App ui{config};
    ui.show();
}
