#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>
#include <tooling/Logger.hpp>

using namespace webfront;

SCENARIO("Logger status shared between translation units") {
    std::list<std::string> logs;
    auto testSink = [&logs](std::string_view text) { logs.emplace_back(text); };
    auto sinkId = log::addSinks(testSink);

    GIVEN("A counting sink") {
        log::setLogLevel(log::Disabled);

        log::info("info log 1");
        log::debug("debug log 2");
        log::warn("warn log 3");
        log::error("error log 4");
        REQUIRE(logs.size() == 0);
    }

    GIVEN("A counting sink") {
        log::setLogLevel(log::Warn);

        log::debug("debug log 5");
        log::warn("warn log 6");
        REQUIRE(logs.size() == 1);
    }
    log::removeSinks(sinkId);
}
