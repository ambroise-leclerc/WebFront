#include <tooling/Logger.hpp>

#include <catch2/catch.hpp>

using namespace webfront;
using namespace std;

SCENARIO("Logger") {
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    log::info("Tests started");
}