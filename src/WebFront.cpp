
#include <HTTPServer.hpp>

#include <filesystem>
#include <iostream>

namespace net = std::experimental::net;

int main() {
    using namespace webfront;
    using namespace webfront::http;
    using namespace std;

    cout << "WebFront launched from " << std::filesystem::current_path().string() << "\n";

    
    Server server("0.0.0.0", "80");
    server.run();

}