#include <webfront/testing/Bdd.hpp>
#include <webfront/frontend/DefaultBrowser.hpp>
#include <webfront/frontend/Url.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace webfront::bdd;

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

const Scenario systemBrowserSession{"The system browser session waits only after a successful launch", [] {
    std::istringstream input("\nremaining");
    std::ostringstream output;
    given("a successful desktop launcher", [&] {
        webfront::browser::open("127.0.0.2", "9002", "index.html?x=1#result", {recordLaunch, input, output});
        require(launchedUrl == "http://127.0.0.2:9002/index.html?x=1#result");
        require(input.peek() == 'r');
        require(output.str().find("Press Enter") != std::string::npos);
    });
    given("the legacy two-argument entry point", [&] {
        webfront::browser::open("9002", "index.html", {recordLaunch, input, output});
        require(launchedUrl == "http://127.0.0.1:9002/index.html");
        require(input.peek() == 'r');
    });
    given("the page used by the WebView2 path-encoding regression", [&] {
        webfront::browser::open("9002", "encoded path %23.html?x=1#example", {recordLaunch, input, output});
        then("the desktop launcher receives the encoded path and the query and fragment intact", [&] {
            require(launchedUrl == "http://127.0.0.1:9002/encoded%20path%20%23.html?x=1#example");
            require(input.peek() == 'r');
        });
    });
    given("a failed desktop launcher", [&] {
        requireThrows([&] { webfront::browser::open("9002", "index.html", {rejectLaunch, input, output}); }, equals("desktop launcher failed"));
        require(input.peek() == '\n');
        require(output.str().empty());
    });
    given("an invalid address", [&] {
        requireThrows<std::invalid_argument>([&] { webfront::browser::open("bad/address", "9002", "index.html", {recordLaunch, input, output}); });
        require(output.str().empty());
    });
}};

const Scenario everyFrontend{"Every frontend builds the application URL the same way", [] {
    given("plain page paths", [&] {
        then("the path is appended to the local origin", [&] {
            require(applicationUrl("9002", "index.html") == "http://127.0.0.1:9002/index.html");
            require(applicationUrl("9002", "demos/react.html") == "http://127.0.0.1:9002/demos/react.html");
            require(applicationUrl("9002", "") == "http://127.0.0.1:9002/");
        });
    });

    given("path separators and a literal backslash", [&] {
        then("URL path separators survive and the backslash is percent-encoded", [&] {
            require(applicationUrl("9002", "folder/sub folder/page.html") == "http://127.0.0.1:9002/folder/sub%20folder/page.html");
            require(applicationUrl("9002", "folder\\page.html") == "http://127.0.0.1:9002/folder%5Cpage.html");
        });
    });

    given("a plus sign in a filename, query and fragment", [&] {
        then("the path plus survives the server decoder without changing query or fragment syntax", [&] {
            require(applicationUrl("9002", "a+b.html?value=a+b#c+d") == "http://127.0.0.1:9002/a%2Bb.html?value=a+b#c+d");
            require(applicationUrl("9002", "a%2Bb.html") == "http://127.0.0.1:9002/a%2Bb.html");
        });
    });

    given("a UTF-8 path containing URL delimiters", [&] {
        then("literal filename delimiters are supplied as percent escapes", [&] {
            require(applicationUrl("9002", "folder/a b%23c%3Fd%25\xc3\xa9.html") == "http://127.0.0.1:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
            require(applicationUrl("9002", "encoded path %23.html") == "http://127.0.0.1:9002/encoded%20path%20%23.html");
        });
        then("quotes and backticks are escaped", [&] {
            require(applicationUrl("9002", "a&b;c`d$e\"f'g.html") == "http://127.0.0.1:9002/a&b;c%60d$e%22f%27g.html");
        });
    });
}};

const Scenario defaultBrowser{"The default browser opens a quoted, encoded URL", [] {
    given("a page whose name contains characters that are special to shells and URLs", [&] {
        const auto command = webfront::browser::command("9002", "my page %231&2.html");

        then("the URL is the shared encoded URL, enclosed in quotes", [&] {
#ifdef _WIN32
            require(command, endsWith("\"http://127.0.0.1:9002/my%20page%20%231&2.html\""));
#else
            require(command, endsWith("http://127.0.0.1:9002/my%20page%20%231&2.html'"));
#endif
            require(command.find(' ', command.find("http://")) == std::string::npos);
        });
    });

    given("a port that is not a decimal number", [&] {
        then("the command is refused instead of reaching a shell", [&] {
            requireThrows<std::invalid_argument>([&] { webfront::browser::command("9002; touch x", "index.html"); });
            requireThrows<std::invalid_argument>([&] { webfront::browser::command("", "index.html"); });
        });
    });
    given("a WSL desktop launch with percent escapes and shell metacharacters", [&] {
        const auto url        = applicationUrl("9002", "a b c.html?value=$HOME&literal=%25");
        const auto invocation = webfront::browser::wslCommandForUrl(url);
        then("PowerShell receives a literal URL without invoking cmd.exe", [&] {
            require(invocation.find("cmd.exe") == std::string::npos);
            require(invocation.find("powershell.exe -NoProfile -NonInteractive -Command ") != std::string::npos);
            require(invocation, endsWith("'Start-Process -FilePath '\\''" + url + "'\\'''"));
            require(webfront::browser::shellArgument("a'b$c") == "'a'\\''b$c'");
        });
    });
}};

const Scenario popupURLs{"Popup URLs remain literal arguments to desktop launchers", [] {
    const std::string url = "https://example.com/it's?value=$HOME&x=1";
    require(
        webfront::browser::wslCommandForUrl(url).find(webfront::browser::shellArgument("Start-Process -FilePath 'https://example.com/it''s?value=$HOME&x=1'"))
        != std::string::npos);
#ifndef _WIN32
    require(webfront::browser::commandForUrl(url).find(webfront::browser::shellArgument(url)) != std::string::npos);
#else
    given("a UTF-8 URL passed to the Windows shell", [&] {
        static std::wstring received;
        auto                execute = [](HWND, LPCWSTR operation, LPCWSTR destination, LPCWSTR, LPCWSTR, INT) -> HINSTANCE {
            received = destination;
            check(std::wstring_view(operation) == L"open");
            return reinterpret_cast<HINSTANCE>(std::intptr_t{33});
        };
        webfront::browser::detail::launchWindowsUrl("https://example.com/caf\xc3\xa9/\xf0\x9f\x9a\x80", execute);
        then("ShellExecuteW receives the accent and supplementary character unchanged", [&] {
            require(received == L"https://example.com/caf\u00e9/\U0001f680");
        });
        then("invalid UTF-8 and shell errors are reported", [&] {
            requireThrows<std::runtime_error>([&] { webfront::browser::detail::launchWindowsUrl("https://example.com/\xc3", execute); });
            auto failedExecute = [](HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) -> HINSTANCE {
                return reinterpret_cast<HINSTANCE>(std::intptr_t{31});
            };
            requireThrows([&] { webfront::browser::detail::launchWindowsUrl("https://example.com/", failedExecute); }, equals("Failed to open system browser (ShellExecute error 31)"));
        });
    });
#endif
}};
