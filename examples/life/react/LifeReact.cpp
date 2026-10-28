#include "LifeReact.hpp"

#include "../LifeFrontend.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: WebFrontLifeReact <project directory>\n";
        return 2;
    }
    try {
        webfront::demo::LifeApp app{argv[1]};
        auto                    subscription = webfront::demo::installLifeReact(app);
        app.bootstrap("developer");
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
