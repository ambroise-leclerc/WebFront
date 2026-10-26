#include <webfront/WebFront.hpp>
#include WEBFRONT_LIFE_ASSETS_HEADER
#include "Life.hpp"
#include "LifeFrontend.hpp"

#include <iostream>

int main() {
    try {
        if constexpr (!webfront::demo::lifeNativeWindow)
            std::cerr << webfront::demo::browserNotice << '\n';
        webfront::demo::LifeAppWithAssets<webfront::fs::LifeUI> app;
        app.cppFunction("life.step", &webfront::demo::nextLife);
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
