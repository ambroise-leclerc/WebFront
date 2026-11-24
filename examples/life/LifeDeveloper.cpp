// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — examples
// Role: Developer stage of the Game of Life walkthrough: the application opens the developer environment on a project.
// Thread: ui-loop | Alloc: unrestricted
// LLM: uses NativeApp::bootstrap("developer"); options live in LifeDeveloperOptions.hpp

#include "../PortOption.hpp"
#include "Life.hpp"
#include "LifeDeveloperOptions.hpp"
#include "LifeFrontend.hpp"
#include <webfront/WebFront.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (!webfront::demo::applyPortOption(argc, argv))
        return 2;
    const auto options = webfront::demo::parseLifeDeveloperOptions(argc, argv);
    if (!options) {
        std::cerr << "Usage: WebFrontLifeDeveloper [--port <n>] [<project> [<environment-tarball-url> <sha512-integrity>]]\n"
                     "Without arguments the project is ./"
                  << webfront::demo::LifeDeveloperOptions::defaultProject << ", created when missing; the port is --port or WEBFRONT_PORT (0: a free port).\n";
        return 2;
    }
    try {
        webfront::demo::prepareLifeProject(options->project);
        webfront::demo::LifeApp app{options->project};
        app.cppFunction("life.step", &webfront::demo::nextLife);
        if (!options->environment)
            app.bootstrap("developer");
        else
            app.bootstrap("developer", {.environment = *options->environment, .integrity = *options->integrity});
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
