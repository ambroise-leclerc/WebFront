#include "LifeReact.hpp"

#include "../LifeDeveloperOptions.hpp"
#include "../LifeFrontend.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && *argv[1] == '\0')) {
        std::cerr << "Usage: WebFrontLifeReact [<project directory>]\n"
                     "Without argument the project is ./"
                  << webfront::demo::LifeDeveloperOptions::defaultProject << ", created when missing.\n";
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
