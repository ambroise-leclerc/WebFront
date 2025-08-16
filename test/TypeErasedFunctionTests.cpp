#include <utils/TypeErasedFunction.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include <iostream>

using namespace std;
using namespace webfront;


size_t testCounter {};
std::string testString {};

void function4(const char* text, int plusValue, int minusValue) {
    testString.append(text);
    testCounter += static_cast<size_t>(plusValue);
    testCounter -= static_cast<size_t>(minusValue);
}

SCENARIO("TypeErasedFunctions") {
    GIVEN("Different free functions, member functions and lambdas") {

        auto function1 = [&](int value) {
            testCounter += static_cast<size_t>(value);
        };

        auto function2 = [&](int value, std::string text) {
            testString += text;
            function1(value);
        };

        class Test {
        public:
            Test(size_t& testC, std::string& testS) : tC(testC), tS(testS) {} 
            void function3(int value, std::string_view text) {
                tC += static_cast<size_t>(value);
                tS += text;
            }
        private:
            size_t& tC;
            std::string& tS;
        };


        WHEN("Stored in a map") {
            std::map<std::string, utils::TypeErasedFunction> functions;

            functions.try_emplace("function1", function1);
            functions.try_emplace("function2", function2);

            Test test(testCounter, testString);
            functions.try_emplace("function3", [&](int value, std::string_view text){
                test.function3(value, text); 
            });

            functions.try_emplace("function4", function4);

            THEN("They can be retrieved uniformly"){
                functions.at("function1")(5);
                REQUIRE(testCounter == 5);

                functions.at("function2")(8, std::string("Sponge"));
                REQUIRE(testCounter == 13);
                REQUIRE(testString == "Sponge");

                REQUIRE_THROWS_AS(functions.at("function2")(8, "Bob"), std::invalid_argument);

                functions.at("function3")(3, std::string_view("Bob"));
                REQUIRE(testCounter == 16);
                REQUIRE(testString == "SpongeBob");

                functions.at("function4")(reinterpret_cast<const char*>(" Square"), 100, 33);
                REQUIRE(testCounter == 83);
                REQUIRE(testString == "SpongeBob Square");


            }
        }
    }
}