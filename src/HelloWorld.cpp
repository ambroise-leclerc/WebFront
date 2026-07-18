#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/FileSystem.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>
#include <WebFront.hpp>
#include <tooling/Logger.hpp>
#include <tooling/PathUtils.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <stdexcept>

using namespace std;
using namespace webfront;


// LCOV_EXCL_START - interactive example; behavior is covered by the browser integration target.
int main(int argc, char** argv) {
    using HelloFS     = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::ReactFS, fs::BabelFS>;
    using WebFrontDbg = BasicWF<NetProvider, HelloFS>;

    const string httpPort = "9002";
    const string mainHtml = argc > 1 ? argv[1] : "module-demo.html";
    auto docRoot  = tooling::findDocRoot(mainHtml);

    cout << "WebFront launched from " << filesystem::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFrontDbg webFront(httpPort, docRoot);
    optional<WebFrontDbg::UI> connectedUI;

    webFront.cppFunction<void, std::string>("print", [](const std::string& text) {
        std::cout << text << '\n';
    });
    webFront.cppFunction<void>("moduleReady", [&connectedUI] {
        if (connectedUI)
            connectedUI->jsFunction("webfrontModule.receiveFromCpp")("Hello from C++ through WebFront");
    });

    const bool useReactExample = filesystem::path(mainHtml).filename() == "react.html";
    webFront.onUIStarted([&connectedUI, useReactExample](WebFrontDbg::UI ui) {
        connectedUI.emplace(ui);
        if (!useReactExample)
            return;

        ui.addScript("var addText = function(text, num) {          \n"
                     "  const print = webFront.cppFunction('print');\n"
                     "  print(text + ' of ' + num);                \n"
                     "}                                            \n");
        ui.jsFunction("addText")("Hello World", 2025);
    });

    webFront.openAndRun(mainHtml);

    log::info("Application shutdown complete.");

    return 0;
}
// LCOV_EXCL_STOP
