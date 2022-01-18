
#include <WebFront.hpp>

#include <filesystem>
#include <iostream>


int main() {
    using namespace std;

    cout << "WebFront launched from " << std::filesystem::current_path().string() << "\n";

    
    webfront::UI ui("80");
    ui.runAsync();


    while (true) {
        std::string input;
        cout << "> ";
        cin >> input;
        cout << input << "\n";
    }


}