#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frontend/DefaultBrowser.hpp>
#include <frontend/Url.hpp>

#include <sstream>

using webfront::frontend::detail::applicationUrl;

namespace {
std::string launchedUrl;
void        recordLaunch(const std::string& url) {
    launchedUrl = url;
}
void rejectLaunch(const std::string&) {
    throw std::runtime_error("desktop launcher failed");
}
}  // namespace

SCENARIO("The system browser session waits only after a successful launch") {
    std::istringstream input("\nremaining");
    std::ostringstream output;
    GIVEN("a successful desktop launcher") {
        webfront::browser::open("127.0.0.2", "9002", "index.html?x=1#result", {recordLaunch, input, output});
        REQUIRE(launchedUrl == "http://127.0.0.2:9002/index.html?x=1#result");
        REQUIRE(input.peek() == 'r');
        REQUIRE(output.str().find("Press Enter") != std::string::npos);
    }
    GIVEN("the legacy two-argument entry point") {
        webfront::browser::open("9002", "index.html", {recordLaunch, input, output});
        REQUIRE(launchedUrl == "http://127.0.0.1:9002/index.html");
        REQUIRE(input.peek() == 'r');
    }
    GIVEN("a failed desktop launcher") {
        REQUIRE_THROWS_WITH(webfront::browser::open("9002", "index.html", {rejectLaunch, input, output}), "desktop launcher failed");
        REQUIRE(input.peek() == '\n');
        REQUIRE(output.str().empty());
    }
    GIVEN("an invalid address") {
        REQUIRE_THROWS_AS(webfront::browser::open("bad/address", "9002", "index.html", {recordLaunch, input, output}), std::invalid_argument);
        REQUIRE(output.str().empty());
    }
}

SCENARIO("Every frontend builds the application URL the same way") {
    GIVEN("plain page paths") {
        THEN("the path is appended to the local origin") {
            REQUIRE(applicationUrl("9002", "index.html") == "http://127.0.0.1:9002/index.html");
            REQUIRE(applicationUrl("9002", "demos/react.html") == "http://127.0.0.1:9002/demos/react.html");
            REQUIRE(applicationUrl("9002", "") == "http://127.0.0.1:9002/");
        }
    }

    GIVEN("a UTF-8 path containing URL delimiters") {
        THEN("literal filename delimiters are supplied as percent escapes") {
            REQUIRE(applicationUrl("9002", "folder/a b%23c%3Fd%25\xc3\xa9.html") == "http://127.0.0.1:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
            REQUIRE(applicationUrl("9002", "encoded path %23.html") == "http://127.0.0.1:9002/encoded%20path%20%23.html");
        }
        THEN("quotes and backticks are escaped") {
            REQUIRE(applicationUrl("9002", "a&b;c`d$e\"f'g.html") == "http://127.0.0.1:9002/a&b;c%60d$e%22f%27g.html");
        }
    }
}

SCENARIO("The default browser opens a quoted, encoded URL") {
    GIVEN("a page whose name contains characters that are special to shells and URLs") {
        const auto command = webfront::browser::command("9002", "my page %231&2.html");

        THEN("the URL is the shared encoded URL, enclosed in quotes") {
#ifdef _WIN32
            REQUIRE_THAT(command, Catch::Matchers::EndsWith("\"http://127.0.0.1:9002/my%20page%20%231&2.html\""));
#else
            REQUIRE_THAT(command, Catch::Matchers::EndsWith("http://127.0.0.1:9002/my%20page%20%231&2.html'"));
#endif
            REQUIRE(command.find(' ', command.find("http://")) == std::string::npos);
        }
    }

    GIVEN("a port that is not a decimal number") {
        THEN("the command is refused instead of reaching a shell") {
            REQUIRE_THROWS_AS(webfront::browser::command("9002; touch x", "index.html"), std::invalid_argument);
            REQUIRE_THROWS_AS(webfront::browser::command("", "index.html"), std::invalid_argument);
        }
    }
    GIVEN("a WSL desktop launch with percent escapes and shell metacharacters") {
        const auto url        = applicationUrl("9002", "a b c.html?value=$HOME&literal=%25");
        const auto invocation = webfront::browser::wslCommandForUrl(url);
        THEN("PowerShell receives a literal URL without invoking cmd.exe") {
            REQUIRE(invocation.find("cmd.exe") == std::string::npos);
            REQUIRE(invocation.find("powershell.exe -NoProfile -NonInteractive -Command ") != std::string::npos);
            REQUIRE_THAT(invocation, Catch::Matchers::EndsWith("'Start-Process -FilePath '\\''" + url + "'\\'''"));
            REQUIRE(webfront::browser::shellArgument("a'b$c") == "'a'\\''b$c'");
        }
    }
}
