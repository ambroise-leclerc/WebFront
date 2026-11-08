#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/automation/Discovery.hpp>
#include <webfront/automation/Enable.hpp>

#include <fstream>
#include <atomic>
#include <functional>
#include <thread>
#include <sstream>

namespace {
using namespace webfront::automation;

struct ConsumerApp {
    struct UI {
        int                                       value;
        std::shared_ptr<std::vector<std::string>> scripts = std::make_shared<std::vector<std::string>>();
        int                                       id() const {
            return value;
        }
        void addScript(std::string text) const {
            scripts->push_back(std::move(text));
        }
        friend bool operator==(const UI& a, const UI& b) {
            return a.value == b.value;
        }
    };
    std::filesystem::path project;
    std::vector<std::string> registered;
    template <typename Handler>
    void cppFunction(std::string_view name, Handler) {
        registered.emplace_back(name);
    }
    webfront::UISubscription subscribeUIStarted(std::function<void(UI)>) {
        return {};
    }
    webfront::UISubscription subscribeUIClosed(std::function<void(UI)>) {
        return {};
    }
    const std::filesystem::path& projectDirectory() const {
        return project;
    }
};

std::string read(const std::filesystem::path& path) {
    std::ifstream       input(path, std::ios::binary);
    std::ostringstream  text;
    text << input.rdbuf();
    return text.str();
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
}

discovery::Entry entry(std::string session, long pid = discovery::currentProcess()) {
    return {.session = std::move(session), .project = "/work/project", .endpoint = "/work/endpoint", .allow = "[\"check\",\"build\"]", .pid = pid};
}

// A process identifier no live process has: far beyond the usual ranges of Windows and POSIX systems.
constexpr long deadProcess = 2147483000L;
}  // namespace

SCENARIO("A shared session is listed with its grants and never with its capability") {
    webfront::test::TemporaryDirectory root;
    GIVEN("a registration in a discovery directory") {
        const auto directory = root.path() / "sessions";
        {
            discovery::Registration registration{directory, entry("alpha")};
            WHEN("the sessions are listed") {
                const auto listed = discovery::list(directory);
                THEN("the session carries its project, grants, process and endpoint") {
                    REQUIRE(listed.size() == 1);
                    REQUIRE(listed[0].session == "alpha");
                    REQUIRE(listed[0].project == "/work/project");
                    REQUIRE(listed[0].endpoint == "/work/endpoint");
                    REQUIRE(listed[0].allow == "[\"check\",\"build\"]");
                    REQUIRE(listed[0].pid == discovery::currentProcess());
                }
                THEN("the printed JSON has the contract's members and no secret") {
                    const Object printed{discovery::listJson(listed), 64000};
                    REQUIRE(printed.raw("version") == "1");
                    REQUIRE(printed.raw("sessions").find("\"state\":\"running\"") != std::string::npos);
                    REQUIRE(printed.raw("sessions").find("token") == std::string::npos);
                    REQUIRE(printed.raw("sessions").find("capability") == std::string::npos);
                }
            }
        }
        THEN("closing the registration withdraws the record") {
            REQUIRE(discovery::list(directory).empty());
            REQUIRE(std::filesystem::is_empty(directory));
        }
    }
    GIVEN("no discovery directory") {
        THEN("nothing is listed and nothing is created") {
            REQUIRE(discovery::list(root.path() / "absent").empty());
            REQUIRE_FALSE(std::filesystem::exists(root.path() / "absent"));
        }
    }
}

SCENARIO("A discovery directory only lists live, well-formed records") {
    webfront::test::TemporaryDirectory root;
    const auto                         directory = root.path() / "sessions";
    discovery::Registration            live{directory, entry("live")};
    GIVEN("a record whose process is gone") {
        discovery::Registration stale{directory, entry("stale", deadProcess)};
        WHEN("the sessions are listed") {
            const auto listed = discovery::list(directory, false);
            THEN("it is not listed, and `prune` decides whether it is removed") {
                REQUIRE(listed.size() == 1);
                REQUIRE(listed[0].session == "live");
                REQUIRE(std::filesystem::exists(directory / "stale.json"));
                REQUIRE(discovery::list(directory).size() == 1);
                REQUIRE_FALSE(std::filesystem::exists(directory / "stale.json"));
            }
        }
    }
    GIVEN("malformed, foreign and mismatched files") {
        write(directory / "garbage.json", "{not json");
        write(directory / "notes.txt", "{}");
        write(directory / "bad name.json", entry("bad").json());
        write(directory / "other.json", entry("someone-else").json());  // the file name must be the session
        write(directory / "huge.json", std::string(discovery::recordLimit + 1, ' '));
        write(directory / "noPid.json", "{\"version\":1,\"session\":\"noPid\",\"project\":\"p\",\"endpoint\":\"e\",\"allow\":[]}");
        WHEN("the sessions are listed") {
            const auto listed = discovery::list(directory);
            THEN("only the real session appears, and the files are left alone") {
                REQUIRE(listed.size() == 1);
                REQUIRE(listed[0].session == "live");
                for (const auto name : {"garbage.json", "notes.txt", "bad name.json", "other.json", "huge.json", "noPid.json"}) {
                    CAPTURE(name);
                    REQUIRE(std::filesystem::exists(directory / name));
                }
            }
        }
    }
    GIVEN("an identifier that is not a plain name") {
        THEN("it is refused before any path is built") {
            for (const auto* id : {"", "../escape", "a/b", "a.b", "with space"})
                REQUIRE_THROWS_AS(discovery::Registration(directory, entry(id)), std::invalid_argument);
            REQUIRE_FALSE(std::filesystem::exists(root.path() / "escape.json"));
        }
    }
}

SCENARIO("A client configuration attaches to an endpoint without grants or secrets") {
    GIVEN("an agent and an endpoint directory") {
        const auto text = discovery::mcpConfiguration("/tools/webfront-agent", "/run/session", "bf");
        THEN("it is a stdio server configuration whose arguments only attach") {
            const Object config{text};
            const Object servers{config.raw("mcpServers")};
            const Object server{servers.raw("bf")};
            REQUIRE(server.string("command") == "/tools/webfront-agent");
            REQUIRE(server.raw("args") == "[\"--attach\",\"/run/session\",\"--mcp\"]");
            REQUIRE(text.find("--allow") == std::string::npos);
            REQUIRE(text.find("token") == std::string::npos);
        }
        THEN("a server name that could break the JSON or a command line is refused") {
            for (const auto* name : {"", "a b", "a\"b", "../x"})
                REQUIRE_THROWS_AS(discovery::mcpConfiguration("agent", "dir", name), std::invalid_argument);
        }
    }
}

SCENARIO("A consumer enables automation explicitly, with fixed grants and a visible endpoint") {
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    ConsumerApp app{.project = root.path() / "project"};
    const auto  sessions = root.path() / "sessions";
    GIVEN("an application that never enables automation") {
        THEN("no automation callback is registered") {
            REQUIRE(app.registered.empty());
        }
    }
    GIVEN("a refused configuration") {
        THEN("a missing endpoint directory, an unknown grant and an endpoint inside the project are all refused") {
            REQUIRE_THROWS_AS(enableAutomation(app, {.grants = {"check"}, .discovery = sessions, .announce = false}), std::invalid_argument);
            REQUIRE_THROWS_AS(enableAutomation(app, {.grants = {"shell"}, .listen = root.path() / "endpoint", .discovery = sessions, .announce = false}),
                              std::invalid_argument);
            REQUIRE_THROWS_AS(enableAutomation(app, {.grants = {"check"}, .listen = root.path() / "project" / "endpoint", .discovery = sessions, .announce = false}),
                              std::invalid_argument);
            REQUIRE_FALSE(std::filesystem::exists(root.path() / "endpoint"));
            REQUIRE(discovery::list(sessions).empty());
        }
    }
    GIVEN("automation enabled with check and inspect") {
        const auto endpoint   = root.path() / "endpoint";
        auto       automation = enableAutomation(app, {.grants = {"check", "inspect"}, .listen = endpoint, .discovery = sessions, .announce = false});
        THEN("the application's own callbacks stay and the automation ones are added") {
            REQUIRE(std::ranges::find(app.registered, "webfront.automation.claim") != app.registered.end());
            REQUIRE(std::ranges::find(app.registered, "webfront.automation.poll") != app.registered.end());
        }
        THEN("the session is discoverable and names the endpoint and the grants") {
            const auto listed = discovery::list(sessions);
            REQUIRE(listed.size() == 1);
            REQUIRE(listed[0].allow == "[\"check\",\"inspect\"]");
            REQUIRE(std::filesystem::equivalent(listed[0].endpoint, endpoint));
            REQUIRE(std::filesystem::exists(endpoint / "endpoint.json"));
            const auto capability = Object(read(endpoint / "endpoint.json")).string("token");
            REQUIRE(capability.size() >= 32);
            REQUIRE(read(sessions / (listed[0].session + ".json")).find(capability) == std::string::npos);
        }
        THEN("the launch page carries the single-use claim and the banner states the grants without a secret") {
            const auto page = automation->launchPage();
            REQUIRE(page.starts_with("webfront-bootstrap.html#webfront-automation="));
            const auto banner = automation->banner();
            REQUIRE(banner.find("check, inspect") != std::string::npos);
            REQUIRE(banner.find("--attach") != std::string::npos);
            REQUIRE(banner.find(page.substr(page.find('=') + 1)) == std::string::npos);
            REQUIRE(automation->windowTitle("Board").find("automation") != std::string::npos);
        }
        THEN("an operation that was not granted is refused and audited") {
            const auto id      = Object(automation->controller()->describe()).string("session");
            const Object request{objectJson({
                {  "session", webfront::json::quote(id)},
                {"operation", webfront::json::quote("build")},
                {"arguments",                         "{}"}
            })};
            REQUIRE_THROWS_AS(automation->controller()->start(request), OperationError);
            REQUIRE(automation->controller()->describe().find("\"denied\"") != std::string::npos);
        }
        THEN("closing revokes the session and withdraws the record and the endpoint, once or repeatedly") {
            automation->close();
            automation->close();
            REQUIRE(automation->controller()->isClosed());
            REQUIRE(discovery::list(sessions).empty());
            REQUIRE_FALSE(std::filesystem::exists(endpoint / "endpoint.json"));
        }
    }
}

namespace {
// A renderer running `while (true) {}` never executes the window.close() the supervisor injects: the page never polls
// again, so the operation's native deadline passes and nothing else ends it.
Object checkRequest(const Session& session) {
    return Object(objectJson({
        {  "session", webfront::json::quote(Object(session.describe()).string("session"))},
        {"operation",                                    webfront::json::quote("check")},
        {"arguments",                                                               "{}"}
    }));
}

template <typename Predicate>
bool eventually(Predicate done, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    return done();
}
}  // namespace

SCENARIO("A window that ignores the deadline's close request is declared unresponsive by the consumer's automation") {
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    ConsumerApp           app{.project = root.path() / "project"};
    const auto            sessions = root.path() / "sessions";
    std::atomic<int>      unresponsive{0};
    const auto            enable = [&](std::chrono::milliseconds grace) {
        return enableAutomation(app,
                                {.grants         = {"check"},
                                 .listen         = root.path() / ("endpoint" + std::to_string(grace.count())),
                                 .discovery      = sessions,
                                 .timeout        = std::chrono::seconds{1},
                                 .closeGrace     = grace,
                                 .onUnresponsive = [&] { ++unresponsive; },
                                 .announce       = false});
    };
    GIVEN("an operation that never completes and a window that never closes") {
        auto automation = enable(std::chrono::milliseconds{100});
        automation->controller()->poll();  // the page is ready, then stops polling
        automation->controller()->start(checkRequest(*automation->controller()));
        THEN("past the deadline and the grace, the record and endpoint are withdrawn and the owner is told") {
            REQUIRE(eventually([&] { return unresponsive.load() == 1; }));
            REQUIRE(automation->controller()->isClosed());
            REQUIRE(discovery::list(sessions).empty());
            REQUIRE_FALSE(std::filesystem::exists(automation->endpoint() / "endpoint.json"));
        }
    }
    GIVEN("a window that does close within the grace after the deadline") {
        auto automation = enable(std::chrono::seconds{30});
        automation->controller()->poll();
        automation->controller()->start(checkRequest(*automation->controller()));
        REQUIRE(eventually([&] { return automation->controller()->isClosed(); }));
        WHEN("the application ends show() and closes the automation") {
            automation->close();
            THEN("it is never declared unresponsive") {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                REQUIRE(unresponsive.load() == 0);
            }
        }
    }
    GIVEN("an attached client's explicit shutdown that a stuck renderer ignores") {
        auto automation = enable(std::chrono::milliseconds{100});
        automation->controller()->poll();
        automation->controller()->start(checkRequest(*automation->controller()));
        // --shutdown closes the session and clears the active operation: its deadline can no longer expire.
        automation->controller()->close("Shutdown requested by an attached client");
        THEN("the window is still declared unresponsive after the grace") {
            REQUIRE(eventually([&] { return unresponsive.load() == 1; }));
            REQUIRE(discovery::list(sessions).empty());
        }
    }
    GIVEN("the window disconnecting by itself") {
        auto automation = enable(std::chrono::milliseconds{50});
        automation->controller()->close("Developer window disconnected");
        THEN("the application is not declared unresponsive, whatever the time it takes to close the automation") {
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            REQUIRE(unresponsive.load() == 0);
        }
    }
}
