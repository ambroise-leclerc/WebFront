#include <catch2/catch_test_macros.hpp>
#include <webfront/testing/Jasmine.hpp>

SCENARIO("Consumer Jasmine configuration is bounded and rejects ambiguous or incomplete input") {
    using webfront::testing::detail::jasmineConfiguration;
    GIVEN("a runtime, a spec and an explicit report destination") {
        const auto config = jasmineConfiguration(R"({"files":["jasmine.js","board.spec.js"],"report":"report.json"})");
        REQUIRE(config.files == std::vector<std::string>{"jasmine.js", "board.spec.js"});
        REQUIRE(config.report == "report.json");
    }
    GIVEN("an invalid or ambiguous configuration") {
        for (const auto text : {R"({"files":["jasmine.js"],"report":"r"})",
                                R"({"files":["j","s"]})",
                                R"({"files":["j","s"],"report":"r","report":"other"})",
                                R"({"files":["j","s"],"report":"r","shell":true})",
                                "{} trailing"})
            REQUIRE_THROWS(jasmineConfiguration(text));
    }
}
