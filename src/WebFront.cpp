
#include <WebFront.hpp>

#include <filesystem>
#include <iostream>


int main(int /*argc*/, char** argv) {
    using namespace std;
    using namespace webfront;
    namespace fs = filesystem;
    cout << "WebFront launched from " << std::filesystem::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::setSinks(log::clogSink);
    log::info("Application started");
    UI ui("80", fs::weakly_canonical(fs::path(argv[0])).parent_path());
    //ui.runAsync();
    ui.run();

    while (true) {
        std::string input;
        cout << "> ";
        cin >> input;
        cout << input << "\n";
    }


}