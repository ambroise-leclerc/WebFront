#include <WebFront.hpp>
#include WEBFRONT_PI_ASSETS_HEADER
#include "Pi.hpp"

#include <iostream>

int main() {
    try {
        webfront::NativeAppWithAssets<webfront::fs::PiUI> app;
        app.cppFunction("pi", &webfront::demo::pi);
        app.show();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
