#include "../driver/WfDriver.hpp"
#include <webfront/testing/Bdd.hpp>

#include <sstream>

#include <atomic>

using namespace webfront::bdd;

namespace {
webfront::testdriver::Options parseDriver(std::initializer_list<std::string> extra) {
    std::vector<std::string> arguments{"webfront-wf", "--project", "project", "--environment", "environment.tgz", "--integrity", "sha512-test"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return webfront::testdriver::parse(static_cast<int>(argv.size()), argv.data());
}
}  // namespace

const Scenario usageBeforeEngine{"A missing project directory is a usage error that main reports before an engine starts", [] {
    given("an options structure naming a directory that does not exist, and one naming a directory that does", [&] {
        webfront::testdriver::Options missing = parseDriver({"list"});
        missing.project                       = std::filesystem::temp_directory_path() / "webfront-no-such-project-628";
        webfront::testdriver::Options present = parseDriver({"list"});
        present.project                       = std::filesystem::temp_directory_path();
        then("only the missing one is refused, with a usage error", [&] {
            requireThrows<webfront::testdriver::UsageError>([&] { webfront::testdriver::requireProject(missing); });
            requireNothrow([&] { webfront::testdriver::requireProject(present); });
        });
    });
}};

const Scenario browserTest{"Browser test diagnostics retain HTTP failures without filling the transcript with bridge traffic", [] {
    std::ostringstream output;
    auto*              previous = std::clog.rdbuf(output.rdbuf());
    const auto         sink     = webfront::testdriver::logRequests();
    webfront::log::info("Received request GET on /scripts.js");
    webfront::log::info("  responding http 404");
    webfront::log::info("onMessage(binary): bridge traffic");
    webfront::log::warn("HTTP asset request failed: denied");
    webfront::log::warn("File response ended its response early: short read");
    webfront::log::warn("WebSocket disconnected unexpectedly");
    webfront::log::removeSinks(sink);
    std::clog.rdbuf(previous);
    webfront::log::setLogLevel(webfront::log::Disabled);
    then("the resource, status and failures remain, but ordinary bridge messages do not", [&] {
        check(output.str().find("GET on /scripts.js") != std::string::npos);
        check(output.str().find("responding http 404") != std::string::npos);
        check(output.str().find("request failed: denied") != std::string::npos);
        check(output.str().find("response early: short read") != std::string::npos);
        check(output.str().find("WebSocket disconnected unexpectedly") != std::string::npos);
        check(output.str().find("bridge traffic") == std::string::npos);
    });
}};

const Scenario driverRepeats{"The driver repeats its close request until the window has closed", [] {
    using namespace std::chrono_literals;
    using webfront::testdriver::closeUntilShown;
    std::stop_source shown;
    std::atomic<int> requests = 0;
    given("a window that only exists after a few requests reached nothing", [&] {
        const auto requestClose = [&] {
            // The third request finds the window; show() then returns.
            if (++requests < 3)
                return false;
            shown.request_stop();
            return true;
        };
        then("the request is repeated until show() returns", [&] {
            require(closeUntilShown(shown.get_token(), requestClose, 1h));
            require(requests == 3);
        });
    });
    given("a window that never closes", [&] {
        then("the driver gives up after its patience", [&] {
            require(!(closeUntilShown(
                shown.get_token(),
                [&] {
                    ++requests;
                    return true;
                },
                30ms)));
            require(requests >= 1);
        });
    });
}};

const Scenario pageEnd{"The page's end releases the driver's close without a time limit", [] {
    using namespace std::chrono_literals;
    webfront::testdriver::DriverDeadline deadline{1h};
    std::stop_source                     shown;
    std::jthread                         page([&] {
        deadline.finish();
    });
    require(!deadline.expired(shown.get_token()));
    require(!shown.stop_requested());
}};

const Scenario driverWebView2{"Driver WebView2 profiles isolate environments served at identical URLs", [] {
    using namespace webfront;
    static_assert(std::is_same_v<testdriver::DriverFrontend<frontend::WebView2Frontend>, frontend::WebView2FrontendWithOptions<testdriver::DriverWebView2Options>>);
    static_assert(std::is_same_v<testdriver::DriverFrontend<frontend::CEFFrontend>, testdriver::DriverCEFFrontend>);
    const auto first  = testdriver::DriverWebView2Options::get("first-session");
    const auto second = testdriver::DriverWebView2Options::get("second-session");
    require(first.userDataFolder == std::filesystem::path("first-session"));
    require(second.userDataFolder != first.userDataFolder);
    require(testdriver::DriverWebView2Options::get(nullptr).userDataFolder.empty());
    require(testdriver::DriverWebView2Options::get("").userDataFolder.empty());
}};

const Scenario singleCommand{"A single-command watchdog has a separate budget after bootstrap", [] {
    using namespace std::chrono_literals;
    given("an overall deadline and a shorter command limit", [&] {
        const auto options = parseDriver({"--timeout", "100", "--command-timeout", "4", "--", "run", "sleep"});
        then("both limits survive argument parsing independently", [&] {
            require(options.timeout == 100s);
            require(options.commandTimeout == 4s);
            require(options.command == std::vector<std::string>{"run", "sleep"});
        });
    });
    given("no separate command limit", [&] {
        require(!parseDriver({"list"}).commandTimeout);
    });
    given("invalid or ambiguous limits", [&] {
        requireThrows<webfront::testdriver::UsageError>([&] { parseDriver({"--command-timeout", "0", "list"}); });
        requireThrows<webfront::testdriver::UsageError>([&] { parseDriver({"--command-timeout", "bad", "list"}); });
    });
}};

const Scenario driverWatchdog{"The driver watchdog keeps a native command limit inside the overall budget", [] {
    using namespace std::chrono_literals;
    using webfront::testdriver::DriverDeadline;
    DriverDeadline deadline{1h};
    deadline.beginCase(1ms);
    std::stop_source fallback;
    std::jthread     cancelIfBroken([&](std::stop_token stop) {
        std::mutex                  mutex;
        std::condition_variable_any wake;
        std::unique_lock            lock{mutex};
        if (!wake.wait_for(lock,
                           stop,
                           10s,
                           [] {
                               return false;
                           })
            && !stop.stop_requested())
            fallback.request_stop();
    });
    require(deadline.expired(fallback.get_token()));
    // Starting a command never extends an already expiring overall limit.
    DriverDeadline overall{1ms};
    overall.beginCase(1h);
    require(overall.expired(fallback.get_token()));
    cancelIfBroken.request_stop();
    deadline.endCase();
    std::stop_source cancelled;
    cancelled.request_stop();
    require(!deadline.expired(cancelled.get_token()));
}};
