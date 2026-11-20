#include <webfront/WebFront.hpp>
#include WEBFRONT_LIFE_ASSETS_HEADER
#include "Life.hpp"
#include "LifeFrontend.hpp"
#include "../PortOption.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (!webfront::demo::applyPortOption(argc, argv) || argc != 1) {
        std::cerr << "Usage: WebFrontLifeEmbedded [--port <n>] (WEBFRONT_PORT=0 asks for a free port)\n";
        return 2;
    }
    try {
        webfront::demo::LifeAppWithAssets<webfront::fs::LifeUI> app;
        app.cppFunction("life.step", &webfront::demo::nextLife);
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
