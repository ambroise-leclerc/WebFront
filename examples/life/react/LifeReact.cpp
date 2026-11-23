// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-27
//
// WebFront — examples
// Role: Game of Life variant with a C++ panel inside a React page.
// Thread: ui-loop | Alloc: unrestricted
// LLM: see docs/REACT.md; entry point of the react variant

#include "LifeReact.hpp"

#include "../../PortOption.hpp"
#include "../LifeDeveloperOptions.hpp"
#include "../LifeFrontend.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (!webfront::demo::applyPortOption(argc, argv))
        return 2;
    if (argc > 2 || (argc == 2 && *argv[1] == '\0')) {
        std::cerr << "Usage: WebFrontLifeReact [--port <n>] [<project directory>]\n"
                     "Without argument the project is ./"
                  << webfront::demo::LifeDeveloperOptions::defaultProject
                  << ", created empty when missing: copy main.tsx and webfront.project.json from examples/life/react/ into it first.\n";
        return 2;
    }
    try {
        const std::filesystem::path project = argc == 2 ? argv[1] : webfront::demo::LifeDeveloperOptions::defaultProject;
        webfront::demo::prepareLifeProject(project);
        webfront::demo::LifeApp app{project};
        auto                    subscription = webfront::demo::installLifeReact(app);
        app.bootstrap("developer");
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
