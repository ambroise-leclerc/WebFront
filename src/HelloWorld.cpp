
#include <WebFront.hpp>

#include <filesystem>
#include <iostream>

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

int main(int /*argc*/, char** argv) {
    using namespace std;
    using namespace webfront;
    namespace fs = std::filesystem;

    auto httpPort = "9002";
    auto httpRoot = fs::weakly_canonical(fs::path(argv[0])).parent_path();

    cout << "WebFront launched from " << fs::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFront webFront(httpPort, httpRoot);

    webFront.cppFunction<void, std::string>("print", [](std::string text) { std::cout << text << '\n'; });
    
    webFront.onUIStarted([](UI ui) {
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
        print("Hello World", 2022);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });

    openInDefaultBrowser(httpPort, "index.html");
    webFront.run();
}