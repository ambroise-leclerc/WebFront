#include <utils/TypeErasedFunction.hpp>

#include <catch2/catch.hpp>

#include <map>
#include <string>

#include <iostream>

using namespace std;
using namespace webfront;

SCENARIO("TypeErasedFunctions") {
    GIVEN("Different free functions, member functions and lambdas") {
        size_t testCounter {};
        std::string testString {};

        auto function1 = [&](int value) {
            testCounter += value;
        };

        auto function2 = [&](int value, std::string text) {
            testString += text;
            function1(value);
        };

        WHEN("Stored in a map") {
            std::map<std::string, utils::TypeErasedFunction> functions;

            functions.try_emplace("function1", function1);
            functions.try_emplace("function2", function2);
            THEN("They can be retrieved uniformly"){
                functions.at("function1")(5);
                REQUIRE(testCounter == 5);

                functions.at("function2")(8, std::string("Bob"));
                REQUIRE(testCounter == 13);
                REQUIRE(testString == "Bob");

                REQUIRE_THROWS_AS(functions.at("function2")(8, "Bob"), std::invalid_argument);
            }
        }
    }
}