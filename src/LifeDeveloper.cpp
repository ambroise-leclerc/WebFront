#include "Life.hpp"
#include <WebFront.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: WebFrontLifeDeveloper <project> [<environment-tarball-url> <sha512-integrity>]\n";
        return 2;
    }
    try {
        webfront::NativeApp app{argv[1]};
        app.cppFunction("life.step", &webfront::demo::nextLife);
        if (argc == 2)
            app.bootstrap("developer");
        else
            app.bootstrap("developer", {.environment = argv[2], .integrity = argv[3]});
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
