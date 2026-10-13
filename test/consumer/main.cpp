#include <WebFront.hpp>

int main(int argc, char**) {
    if (argc > 1) {  // never taken by the smoke test: it only proves that the library compiles and links
        webfront::WebFront app;
        app.show();
    }
    return 0;
}
