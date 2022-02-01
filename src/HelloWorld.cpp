
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
    log::info("Application started");
    WebFront webFront("80", fs::weakly_canonical(fs::path(argv[0])).parent_path());
    webFront.onUIStarted([](UI /*ui*/) {
        log::info("UIStarted");
    });

    webFront.run();
    
    /*

    // Hello World, the hard way
    webFront.onUIStarted([](UI document)){
        auto newDiv = document.createElement("div");
        auto newContent = document.createTextNode("Hello World !");
        newDiv.appendChild(newContent);
        document.body.insert(newDiv);
    });

    // Hello World, the convoluted javascript way
    webFront.onUIStarted([](UI ui)){
        ui.addScript("
            function addText(text) {
                let newDiv = document.createElement('div');
                let newContent = document.createTextNode(text);
                newDiv.appendChild(newContent);
                document.body.insert(newDiv); 
            }
        ");
        auto print = ui.functionJS("addText");
        print("Hello World !");
    });

*/
    while (true) {
        string input;
        cout << "> ";
        cin >> input;
        cout << input << "\n";
    }


}