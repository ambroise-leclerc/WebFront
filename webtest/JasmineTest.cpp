#include <WebFront.hpp>

#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
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

using namespace std;
using namespace webfront;

// Find the DocRoot path containing the given file (look for the filename in current directory,
// in temp directory then in sources directory
//
// @param filename
// @return DocRoot path
filesystem::path findDocRoot(string filename) {
    using namespace filesystem;

    log::info("Looking for {} in {}", filename, current_path().string());
    if (exists(current_path() / filename)) return current_path();

    log::info("  not found : Looking now in temp directory {} ", temp_directory_path().string());
    if (exists(temp_directory_path() / filename)) return temp_directory_path();

    path webfront, cp{current_path()};
    for (auto element = cp.begin(); element != cp.end(); ++element) {
        webfront = webfront / *element;
        if (*element == "WebFront") {
            webfront = webfront / "webtest";
            log::info("  not found : Looking now in source directory {}", webfront.string());
            if (exists(webfront / filename)) return webfront;
        }
    }

    throw runtime_error("Cannot find " + filename + " file");
}

int main() {
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);

    std::string_view httpPort{"9002"};
    using Jas = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::JasmineFS>;

    try {
        auto specRunnerFile = "SpecRunner.html";
        auto docRoot = findDocRoot(specRunnerFile);

        using WF = BasicWF<NetProvider, Jas>;
        WF webFront(httpPort, docRoot);
        webFront.cppFunction<std::string, std::string>("getVersion", [](std::string) { return webfront::version; });
        webFront.onUIStarted([](WF::UI ui) {
            log::info("UI Started");
            ui.addScript("var addText = function(text, num) {                 \n"
                     "  displayText(text + ' of ' + num);                       \n"
                     "}                                                   \n");
                     auto displayText = ui.jsFunction("addText");
                     displayText("Hello World", 2023);
        });
        openInDefaultBrowser(httpPort, specRunnerFile);

        log::debug("WebFront started on port {} with docRoot at {}", httpPort, docRoot.string());
        webFront.run();
    }
    catch (std::runtime_error& e) {
        log::error("runtime_error : {}", e.what());
    }
    return 0;
}