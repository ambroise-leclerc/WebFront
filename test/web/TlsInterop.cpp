#include <webfront/tls/WebFront.hpp>

#include <iostream>
#include <latch>

int main(int argc, char** argv) {
    if (argc != 3)
        return 1;
    webfront::tls::WebFront<> app({.port = "0", .tlsCredentials = std::make_shared<webfront::tls::Credentials>(argv[1], argv[2])});
    std::latch                reply{1};
    const auto                post = [&](const webfront::http::RouteRequest&) {
        std::cout << "post" << std::endl;
        reply.wait();
        return webfront::http::RouteReply{};
    };
    const auto   route = app.addRoute("/post/", post, {.postBodyLimit = 64});
    std::jthread server([&] {
        app.run();
    });
    std::cout << app.port() << std::endl;
    std::cin.get();
    reply.count_down();
    std::cin.get();
    app.stop();
    server.join();
    app.shutdown();
}
