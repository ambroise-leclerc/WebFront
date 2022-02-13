
#include <WebFront.hpp>

#include <filesystem>
#include <iostream>

int main(int /*argc*/, char** argv) {
    using namespace std;
    using namespace webfront;
    namespace fs = filesystem;
    cout << "WebFront launched from " << fs::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFront webFront("9002", fs::weakly_canonical(fs::path(argv[0])).parent_path());

    webFront.cppFunction<void, std::string>("print", [](std::string text) { std::cout << text << '\n'; });

    webFront.onUIStarted([](UI ui) {
        ui.addScript("var addText = function(text, num) {                 "
                     "  let print = webFront.cppFunction('print');        "
                     "  print(text + ' of ' + num);                       "
                     "}                                                   ");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
    });

    webFront.run();
}