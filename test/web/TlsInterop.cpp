#include <webfront/tls/WebFront.hpp>

#include <iostream>
#include <latch>
#include <thread>

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
    const auto route = app.addRoute("/post/", post, {.postBodyLimit = 64});
    // Publish the port before starting the worker, and keep stdin from flushing cout
    // while the POST handler is writing its marker.
    std::cout << app.port() << std::endl;
    std::cin.tie(nullptr);
    std::jthread server([&] {
        app.run();
    });
    std::cin.get();
    reply.count_down();
    std::cin.get();
    app.stop();
    server.join();
    app.shutdown();
}
