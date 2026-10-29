#include "Life.hpp"
#include "LifeFrontend.hpp"
#include "LifeDeveloperOptions.hpp"
#include <webfront/WebFront.hpp>

#include <iostream>

int main(int argc, char** argv) {
    const auto options = webfront::demo::parseLifeDeveloperOptions(argc, argv);
    if (!options) {
        std::cerr << "Usage: WebFrontLifeDeveloper [<project> [<environment-tarball-url> <sha512-integrity>]]\n"
                     "Without arguments the project is ./"
                  << webfront::demo::LifeDeveloperOptions::defaultProject << ", created when missing.\n";
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
