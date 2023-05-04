#include <WebFront.hpp>
#include <system/JasmineFS.hpp>


#include <string_view>

/// Open the web UI in the system's default browser
auto openInDefaultBrowser(std::string_view port, std::string_view file) {
#ifdef _WIN32
    auto command = std::string("start ");
#elif __linux__
    auto command = std::string("xdg-open ");
#elif __APPLE__
    auto command = std::string("open ");
#endif

    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    return ::system(command.append(url).c_str());
}


int main() {
    using namespace webfront;

    std::string_view httpPort{"9002"};
    using Jas = filesystem::Multi<filesystem::IndexFS, filesystem::JasmineFS>;
    BasicWF<NetProvider, Jas> webFront(httpPort);
    openInDefaultBrowser(httpPort, "index.html");

    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    log::debug("WebFront started on port {}", httpPort);
    webFront.run();

    return 0;
}