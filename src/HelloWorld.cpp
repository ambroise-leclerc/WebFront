﻿
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
    WebFront webFront("80", fs::weakly_canonical(fs::path(argv[0])).parent_path());
  
  
    webFront.onUIStarted([](UI ui) {
        ui.addScript("                                                          "
        "    var addText = function(text, num) {                                "
        "        let newDiv = document.createElement('div');                    "
        "        let newContent = document.createTextNode(text + ' of ' + num); "
        "        newDiv.appendChild(newContent);                                "
        "        document.body.appendChild(newDiv);                             "
        "    }                                                                  "
        );
        auto print = ui.jsFunction("addText");
        print("Hello World", 2022);
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
*/
    while (true) {
        string input;
        cout << "> ";
        cin >> input;
        cout << input << "\n";
    }


}