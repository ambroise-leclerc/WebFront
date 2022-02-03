#include <catch2/catch.hpp>


#include <array>

#include <iostream>


enum class Type : uint8_t {
    boolean, number, string, smallArray, bigarray,
    
};

class JSFunction {
public:
    static constexpr size_t maxParamsCount = 32;
    static constexpr size_t maxParamsDataSize = 5;  // 1 byte for type, 1-4 bytes for value
    std::array<std::byte, maxParamsCount * maxParamsDataSize> buffer;

    void operator()(auto&&... ts) {
        
        ((std::cout << ts << '\n'),...);
    }
};




using namespace std;
//using namespace webfront;

SCENARIO("JSFunction") {


    JSFunction addText;

    addText("Texte", 45);
}