#include <catch2/catch.hpp>

#include <JsFunction.hpp>


using namespace webfront;
using namespace std;

SCENARIO("JsFunction") {
    JsFunction print("print");
    std::string text{"Bob l'éponge"};
    const char* text2 = "Text2";

   // int data[] = { 1, 2, 3};

    print(true, "Texte", 45, text, text2/*, data, &data[0]*/);

}