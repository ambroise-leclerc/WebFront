// Several applications at once, none with a configured port: each one starts, is reachable on the port it announces, and stops.
// `multi_instance <count>`; the first application takes 9002 when it is free (the test holds that port's lock), the others get
// free ports.
#include <webfront/WebFront.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Net = webfront::NetProvider;

struct Instance {
    webfront::WebFront app;
    std::thread        server{[this] { app.run(); }};
    ~Instance() {
        app.stop();
        if (server.joinable())
            server.join();
    }
};

std::string firstLine(const std::string& port) {
    Net::IoContext context;
    Net::Socket    client(context);
    client.connect(Net::Resolve("127.0.0.1", port));
    const std::string request = "GET /WebFront.js HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\nConnection: close\r\n\r\n";
    client.send(Net::Buffer(request.data(), request.size()));
    std::array<char, 256> buffer{};
    std::string           head;
    while (head.find("\r\n") == std::string::npos) {  // a reception may end inside the status line
        const auto size = client.receive(Net::Buffer(buffer));
        if (size == 0)
            break;
        head.append(buffer.data(), size);
    }
    return head.substr(0, head.find("\r\n"));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("usage: multi_instance <count>");
        const int count = std::stoi(argv[1]);
#ifdef _WIN32
        _putenv_s("WEBFRONT_PORT", "");
#else
        unsetenv("WEBFRONT_PORT");
#endif
        std::vector<std::unique_ptr<Instance>> instances;
        std::set<std::string>                  ports;
        for (int i = 0; i < count; ++i) {
            instances.push_back(std::make_unique<Instance>());
            const std::string port{instances.back()->app.port()};
            if (port == "0" || !ports.insert(port).second)
                throw std::runtime_error("instance " + std::to_string(i) + " announces the port " + port + " twice or without a number");
        }
        for (const auto& instance : instances) {
            const auto line = firstLine(std::string(instance->app.port()));
            if (line != "HTTP/1.1 200 OK")
                throw std::runtime_error("port " + std::string(instance->app.port()) + " answered '" + line + "'");
        }
        instances.clear();  // stops and joins each one
        std::cout << count << " instances started, answered and stopped\n";
    } catch (const std::exception& error) {
        std::cerr << "multi_instance: " << error.what() << '\n';
        return 1;
    }
}
