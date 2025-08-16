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
#include <string>
#include <stdexcept>

using namespace std;
using namespace webfront;


int main(int /*argc*/, char** /*argv*/) {
    using HelloFS     = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::ReactFS, fs::BabelFS>;
    using WebFrontDbg = BasicWF<NetProvider, HelloFS>;

    const string httpPort = "9002";
    const string mainHtml = "react.html";
    auto docRoot  = tooling::findDocRoot(mainHtml);

    cout << "WebFront launched from " << filesystem::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFrontDbg webFront(httpPort, docRoot);

    webFront.cppFunction<void, std::string>("print", [](const std::string& text) {
        std::cout << text << '\n';
    });
    webFront.onUIStarted([](WebFrontDbg::UI ui) {
        ui.addScript("var addText = function(text, num) {                 \n"
                     "  let print = webFront.cppFunction('print');        \n"
                     "  print(text + ' of ' + num);                       \n"
                     "}                                                   \n"
                     "                                                    \n"
                     "var testFunc = function(text) {                     \n"
                     "  let bigText = 'bigText : ' + text + text + ' - '; \n"
                     "  bigText += bigText + bigText;                     \n"
                     "  let cppTest = webFront.cppFunction('cppTest');    \n"
                     "  cppTest(text, bigText, bigText.length);           \n"
                     "}                                                   \n");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2025);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });

    webFront.openAndRun(mainHtml);

    log::info("Application shutdown complete.");

    return 0;
}