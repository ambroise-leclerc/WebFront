#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <webfront/system/SafePath.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace std;
using namespace webfront;

SCENARIO("Paths from the browser stay inside their directory") {
    GIVEN("a relative path of plain components") {
        THEN("it is accepted, component by component") {
            REQUIRE(fs::safeRelativePath("dist/assets/app.js") == filesystem::path("dist") / "assets" / "app.js");
            REQUIRE(fs::safeRelativePath("ui/main.tsx") == filesystem::path("ui") / "main.tsx");
        }
    }
    GIVEN("unsafe paths") {
        auto path = GENERATE(as<string>{},
                             "",
                             "/etc/passwd",
                             "../x",
                             "a/../../x",
                             "a/./b",
                             "a//b",
                             "a/",
                             "..",
                             "...",
                             "C:/x",
                             "c:x",
                             "a\\..\\b",
                             "a?.js",
                             "a*.js",
                             "a<b",
                             "a>b",
                             "a|b",
                             "a\"b",
                             "main..html",
                             "a/b..c/d",
                             string("a\0b", 3),
                             string(4097, 'a'));
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::safeRelativePath(path), invalid_argument);
        }
    }
}
