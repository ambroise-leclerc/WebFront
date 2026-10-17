#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frontend/DefaultBrowser.hpp>
#include <frontend/Url.hpp>

using webfront::frontend::detail::applicationUrl;

SCENARIO("Every frontend builds the application URL the same way") {
    GIVEN("plain page paths") {
        THEN("the path is appended to the local origin") {
            REQUIRE(applicationUrl("9002", "index.html") == "http://localhost:9002/index.html");
            REQUIRE(applicationUrl("9002", "demos/react.html") == "http://localhost:9002/demos/react.html");
            REQUIRE(applicationUrl("9002", "") == "http://localhost:9002/");
        }
    }

    GIVEN("a UTF-8 path containing URL delimiters") {
        THEN("only separators and unreserved characters remain literal") {
            REQUIRE(applicationUrl("9002", "folder/a b#c?d%\xc3\xa9.html") == "http://localhost:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
            REQUIRE(applicationUrl("9002", "encoded path #.html") == "http://localhost:9002/encoded%20path%20%23.html");
        }
        THEN("characters with meaning to shells are escaped") {
            REQUIRE(applicationUrl("9002", "a&b;c`d$e\"f'g.html") == "http://localhost:9002/a%26b%3Bc%60d%24e%22f%27g.html");
        }
    }
}

SCENARIO("The default browser opens a quoted, encoded URL") {
    GIVEN("a page whose name contains characters that are special to shells and URLs") {
        const auto command = webfront::browser::command("9002", "my page #1&2.html");

        THEN("the URL is the shared encoded URL, enclosed in quotes") {
            REQUIRE_THAT(command, Catch::Matchers::EndsWith("\"http://localhost:9002/my%20page%20%231%262.html\""));
            REQUIRE(command.find(' ', command.find("http://")) == std::string::npos);
        }
    }

    GIVEN("a port that is not a decimal number") {
        THEN("the command is refused instead of reaching a shell") {
            REQUIRE_THROWS_AS(webfront::browser::command("9002; touch x", "index.html"), std::invalid_argument);
            REQUIRE_THROWS_AS(webfront::browser::command("", "index.html"), std::invalid_argument);
        }
    }
}
