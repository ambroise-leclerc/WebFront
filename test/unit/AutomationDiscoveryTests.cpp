#include "TemporaryDirectory.hpp"
#include "Bdd.hpp"
#include <webfront/automation/Discovery.hpp>
#include <webfront/automation/Enable.hpp>

#include <fstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <sstream>

using namespace webfront::bdd;

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
    std::vector<std::string> registered{};
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

const Scenario recordReader{"A record that a reader holds open is still withdrawn", [] {
    webfront::test::TemporaryDirectory root;
    given("a registration whose record is open in a reader, as `list` reads it", [&] {
        const auto directory = root.path() / "sessions";
        discovery::Registration registration{directory, entry("alpha")};
        const auto record = directory / "alpha.json";
        auto reader = std::make_unique<std::ifstream>(record, std::ios::binary);
        require(reader->is_open());
        // Windows refuses to delete a file that is open without shared deletion: the withdrawal waits for the reader.
        std::thread release{[&reader] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            reader.reset();
        }};
        when("the session is withdrawn while the reader still holds it", [&] {
            registration.remove();
            release.join();
            then("the record is gone and no session is listed", [&] {
                require(!std::filesystem::exists(record));
                require(discovery::list(directory).empty());
            });
        });
    });
}};

const Scenario sharedSession{"A shared session is listed with its grants and never with its capability", [] {
    webfront::test::TemporaryDirectory root;
    given("a registration in a discovery directory", [&] {
        const auto directory = root.path() / "sessions";
        {
            discovery::Registration registration{directory, entry("alpha")};
            when("the sessions are listed", [&] {
                const auto listed = discovery::list(directory);
                then("the session carries its project, grants, process and endpoint", [&] {
                    require(listed.size() == 1);
                    require(listed[0].session == "alpha");
                    require(listed[0].project == "/work/project");
                    require(listed[0].endpoint == "/work/endpoint");
                    require(listed[0].allow == "[\"check\",\"build\"]");
                    require(listed[0].pid == discovery::currentProcess());
                });
                then("the printed JSON has the contract's members and no secret", [&] {
                    const Object printed{discovery::listJson(listed), 64000};
                    require(printed.raw("version") == "1");
                    require(printed.raw("sessions").find("\"state\":\"running\"") != std::string::npos);
                    require(printed.raw("sessions").find("token") == std::string::npos);
                    require(printed.raw("sessions").find("capability") == std::string::npos);
                });
            });
        }
        then("closing the registration withdraws the record", [&] {
            require(discovery::list(directory).empty());
            require(std::filesystem::is_empty(directory));
        });
    });
    given("no discovery directory", [&] {
        then("nothing is listed and nothing is created", [&] {
            require(discovery::list(root.path() / "absent").empty());
            require(!std::filesystem::exists(root.path() / "absent"));
        });
    });
#ifndef _WIN32
    given("a new discovery directory", [&] {
        const auto directory = root.path() / "sessions";
        discovery::Registration registration{directory, entry("alpha")};
        then("it is created private to the user", [&] {
            struct stat info{};
            require(::stat(directory.c_str(), &info) == 0);
            require((info.st_mode & 0777) == 0700);
        });
    });
    given("a new discovery directory written with a trailing separator", [&] {
        const auto directory = root.path() / "slash";
        discovery::Registration registration{std::filesystem::path{directory.string() + "/"}, entry("alpha")};
        then("it is still new, hence private, and not refused as an existing shared one", [&] {
            struct stat info{};
            require(::stat(directory.c_str(), &info) == 0);
            require((info.st_mode & 0777) == 0700);
        });
    });
    given("an existing directory that other users can read", [&] {
        const auto directory = root.path() / "shared";
        std::filesystem::create_directory(directory);
        require(::chmod(directory.c_str(), 0755) == 0);
        std::ofstream{directory / "previous.txt"} << "dropped earlier";
        then("it is refused, with its permissions and files left as they were", [&] {
            requireThrows<std::runtime_error>([&] { discovery::Registration(directory, entry("alpha")); });
            struct stat info{};
            require(::stat(directory.c_str(), &info) == 0);
            require((info.st_mode & 0777) == 0755);
            require(std::filesystem::exists(directory / "previous.txt"));
            require(!std::filesystem::exists(directory / "alpha.json"));
        });
    });
    given("an existing directory that is already private", [&] {
        const auto directory = root.path() / "private";
        std::filesystem::create_directory(directory);
        require(::chmod(directory.c_str(), 0700) == 0);
        then("it is used", [&] {
            discovery::Registration registration{directory, entry("alpha")};
            require(discovery::list(directory).size() == 1);
        });
    });
#endif
}};

const Scenario discoveryDirectory{"A discovery directory only lists live, well-formed records", [] {
    webfront::test::TemporaryDirectory root;
    const auto                         directory = root.path() / "sessions";
    discovery::Registration            live{directory, entry("live")};
    given("a record whose process is gone", [&] {
        discovery::Registration stale{directory, entry("stale", deadProcess)};
        when("the sessions are listed", [&] {
            const auto listed = discovery::list(directory, false);
            then("it is not listed, and `prune` decides whether it is removed", [&] {
                require(listed.size() == 1);
                require(listed[0].session == "live");
                require(std::filesystem::exists(directory / "stale.json"));
                require(discovery::list(directory).size() == 1);
                require(!std::filesystem::exists(directory / "stale.json"));
            });
        });
    });
    given("malformed, foreign and mismatched files", [&] {
        write(directory / "garbage.json", "{not json");
        write(directory / "notes.txt", "{}");
        write(directory / "bad name.json", entry("bad").json());
        write(directory / "other.json", entry("someone-else").json());  // the file name must be the session
        write(directory / "huge.json", std::string(discovery::recordLimit + 1, ' '));
        write(directory / "noPid.json", "{\"version\":1,\"session\":\"noPid\",\"project\":\"p\",\"endpoint\":\"e\",\"allow\":[]}");
        when("the sessions are listed", [&] {
            const auto listed = discovery::list(directory);
            then("only the real session appears, and the files are left alone", [&] {
                require(listed.size() == 1);
                require(listed[0].session == "live");
                for (const auto name : {"garbage.json", "notes.txt", "bad name.json", "other.json", "huge.json", "noPid.json"}) {
                    auto scope = info(name);
                    require(std::filesystem::exists(directory / name));
                }
            });
        });
    });
    given("an identifier that is not a plain name", [&] {
        then("it is refused before any path is built", [&] {
            for (const auto* id : {"", "../escape", "a/b", "a.b", "with space"})
                requireThrows<std::invalid_argument>([&] { discovery::Registration(directory, entry(id)); });
            require(!std::filesystem::exists(root.path() / "escape.json"));
        });
    });
}};

const Scenario clientConfiguration{"A client configuration attaches to an endpoint without grants or secrets", [] {
    given("an agent and an endpoint directory", [&] {
        const auto text = discovery::mcpConfiguration("/tools/webfront-agent", "/run/session", "bf");
        then("it is a stdio server configuration whose arguments only attach", [&] {
            const Object config{text};
            const Object servers{config.raw("mcpServers")};
            const Object server{servers.raw("bf")};
            require(server.string("command") == "/tools/webfront-agent");
            require(server.raw("args") == "[\"--attach\",\"/run/session\",\"--mcp\"]");
            require(text.find("--allow") == std::string::npos);
            require(text.find("token") == std::string::npos);
        });
        then("a server name that could break the JSON or a command line is refused", [&] {
            for (const auto* name : {"", "a b", "a\"b", "../x"})
                requireThrows<std::invalid_argument>([&] { discovery::mcpConfiguration("agent", "dir", name); });
        });
    });
}};

const Scenario consumerEnables{"A consumer enables automation explicitly, with fixed grants and a visible endpoint", [] {
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    ConsumerApp app{.project = root.path() / "project"};
    const auto  sessions = root.path() / "sessions";
    given("an application that never enables automation", [&] {
        then("no automation callback is registered", [&] {
            require(app.registered.empty());
        });
    });
    given("a refused configuration", [&] {
        then("a missing endpoint directory, an unknown grant and an endpoint inside the project are all refused", [&] {
            requireThrows<std::invalid_argument>([&] { enableAutomation(app, {.grants = {"check"}, .discovery = sessions, .announce = false}); });
            requireThrows<std::invalid_argument>([&] { enableAutomation(app, {.grants = {"shell"}, .listen = root.path() / "endpoint", .discovery = sessions, .announce = false}); });
            requireThrows<std::invalid_argument>([&] { enableAutomation(app, {.grants = {"check"}, .listen = root.path() / "project" / "endpoint", .discovery = sessions, .announce = false}); });
            require(!std::filesystem::exists(root.path() / "endpoint"));
            require(discovery::list(sessions).empty());
            // A refused activation leaves nothing behind in the application: no callback without an object to close them.
            require(app.registered.empty());
        });
        then("a discovery directory that is refused after the endpoint was accepted registers nothing either", [&] {
#ifndef _WIN32
            const auto shared = root.path() / "shared";
            std::filesystem::create_directory(shared);
            require(::chmod(shared.c_str(), 0755) == 0);
            requireThrows([&] { enableAutomation(app, {.grants = {"check"}, .listen = root.path() / "endpoint2", .discovery = shared, .announce = false}); });
            require(app.registered.empty());
#endif
        });
    });
    given("automation enabled with check and inspect", [&] {
        const auto endpoint   = root.path() / "endpoint";
        auto       automation = enableAutomation(app, {.grants = {"check", "inspect"}, .listen = endpoint, .discovery = sessions, .announce = false});
        then("the application's own callbacks stay and the automation ones are added", [&] {
            require(std::ranges::find(app.registered, "webfront.automation.claim") != app.registered.end());
            require(std::ranges::find(app.registered, "webfront.automation.poll") != app.registered.end());
        });
        then("the session is discoverable and names the endpoint and the grants", [&] {
            const auto listed = discovery::list(sessions);
            require(listed.size() == 1);
            require(listed[0].allow == "[\"check\",\"inspect\"]");
            require(std::filesystem::equivalent(listed[0].endpoint, endpoint));
            require(std::filesystem::exists(endpoint / "endpoint.json"));
            const auto capability = Object(read(endpoint / "endpoint.json")).string("token");
            require(capability.size() >= 32);
            require(read(sessions / (listed[0].session + ".json")).find(capability) == std::string::npos);
        });
        then("the launch page carries the single-use claim and the banner states the grants without a secret", [&] {
            const auto page = automation->launchPage();
            require(page.starts_with("webfront-bootstrap.html#webfront-automation="));
            const auto banner = automation->banner();
            require(banner.find("check, inspect") != std::string::npos);
            require(banner.find("--attach") != std::string::npos);
            require(banner.find(page.substr(page.find('=') + 1)) == std::string::npos);
            require(automation->windowTitle("Board").find("automation") != std::string::npos);
        });
        then("an operation that was not granted is refused and audited", [&] {
            const auto id      = Object(automation->controller()->describe()).string("session");
            const Object request{objectJson({
                {  "session", webfront::json::quote(id)},
                {"operation", webfront::json::quote("build")},
                {"arguments",                         "{}"}
            })};
            requireThrows<OperationError>([&] { automation->controller()->start(request); });
            require(automation->controller()->describe().find("\"denied\"") != std::string::npos);
        });
        then("closing revokes the session and withdraws the record and the endpoint, once or repeatedly", [&] {
            automation->close();
            automation->close();
            require(automation->controller()->isClosed());
            require(discovery::list(sessions).empty());
            require(!std::filesystem::exists(endpoint / "endpoint.json"));
        });
    });
}};

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

struct SupervisorClock {
    using Clock = std::chrono::steady_clock;
    inline static std::atomic<Clock::duration::rep> ticks{0}, sampled{0};
    inline static std::atomic<unsigned>             samples{0};
    static void                                     reset() {
        ticks   = Clock::now().time_since_epoch().count();
        sampled = 0;
        samples = 0;
    }
    static Clock::time_point value() {
        return Clock::time_point{Clock::duration{ticks.load()}};
    }
    static Clock::time_point now() {
        const auto tick = ticks.load();
        sampled         = tick;
        ++samples;
        return Clock::time_point{Clock::duration{tick}};
    }
};

void advanceSupervisor(std::chrono::milliseconds amount) {
    const auto delta = std::chrono::duration_cast<SupervisorClock::Clock::duration>(amount).count();
    const auto tick  = SupervisorClock::ticks.fetch_add(delta) + delta;
    requireEventually([&] {
        return SupervisorClock::sampled == tick;
    });
}

void awaitCloseGrace(const auto& automation) {
    requireEventually([&] {
        return automation->controller()->isClosed();
    });
    const auto before = SupervisorClock::samples.load();
    // At most three reads precede the grace loop (operation expiry, startup expiry, grace deadline).
    // Observe a read inside that loop before advancing time: the deadline has captured the old tick.
    requireEventually([&] {
        return SupervisorClock::samples >= before + 4;
    });
}
}  // namespace

const Scenario windowIgnores{"A window that ignores the deadline's close request is declared unresponsive by the consumer's automation", [] {
    SupervisorClock::reset();
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    ConsumerApp      app{.project = root.path() / "project"};
    const auto       sessions = root.path() / "sessions";
    std::atomic<int> unresponsive{0};
    const auto       enable = [&](std::chrono::milliseconds grace, std::chrono::seconds startup = std::chrono::seconds{120}) {
        return std::make_unique<Automation<ConsumerApp, SupervisorClock>>(
            app,
            AutomationOptions{.grants         = {"check"},
                              .listen         = root.path() / ("endpoint" + std::to_string(grace.count())),
                              .discovery      = sessions,
                              .timeout        = std::chrono::seconds{1},
                              .startupTimeout = startup,
                              .closeGrace     = grace,
                              .onUnresponsive =
                                  [&] {
                                      ++unresponsive;
                                  },
                              .announce = false});
    };
    given("an operation that never completes and a window that never closes", [&] {
        auto automation = enable(std::chrono::milliseconds{100});
        automation->controller()->poll();  // the page is ready, then stops polling
        automation->controller()->start(checkRequest(*automation->controller()), SupervisorClock::value());
        then("past the deadline and the grace, the record and endpoint are withdrawn and the owner is told", [&] {
            advanceSupervisor(std::chrono::milliseconds{999});
            require(!automation->controller()->isClosed());
            advanceSupervisor(std::chrono::milliseconds{1});
            awaitCloseGrace(automation);
            advanceSupervisor(std::chrono::milliseconds{99});
            require(unresponsive.load() == 0);
            advanceSupervisor(std::chrono::milliseconds{1});
            requireEventually([&] {
                return unresponsive.load() == 1;
            });
            require(automation->controller()->isClosed());
            require(discovery::list(sessions).empty());
            require(!std::filesystem::exists(automation->endpoint() / "endpoint.json"));
        });
    });
    given("a window that does close within the grace after the deadline", [&] {
        auto automation = enable(std::chrono::seconds{30});
        automation->controller()->poll();
        automation->controller()->start(checkRequest(*automation->controller()), SupervisorClock::value());
        advanceSupervisor(std::chrono::seconds{1});
        awaitCloseGrace(automation);
        when("the application ends show() and closes the automation", [&] {
            automation->close();
            then("it is never declared unresponsive", [&] {
                // close() joined the supervisor: a later callback is impossible, without a timed observation.
                require(unresponsive.load() == 0);
            });
        });
    });
    given("an attached client's explicit shutdown that a stuck renderer ignores", [&] {
        auto automation = enable(std::chrono::milliseconds{100});
        automation->controller()->poll();
        automation->controller()->start(checkRequest(*automation->controller()), SupervisorClock::value());
        // --shutdown closes the session and clears the active operation: its deadline can no longer expire.
        automation->controller()->close("Shutdown requested by an attached client");
        then("the window is still declared unresponsive after the grace", [&] {
            awaitCloseGrace(automation);
            advanceSupervisor(std::chrono::milliseconds{100});
            requireEventually([&] {
                return unresponsive.load() == 1;
            });
            require(discovery::list(sessions).empty());
        });
    });
    given("a renderer blocked before its first poll, so that no operation can expire", [&] {
        auto automation = enable(std::chrono::milliseconds{100}, std::chrono::seconds{1});
        then("the startup deadline revokes the session and the unresponsive window is declared after the grace", [&] {
            advanceSupervisor(std::chrono::milliseconds{999});
            require(!automation->controller()->isClosed());
            advanceSupervisor(std::chrono::milliseconds{1});
            awaitCloseGrace(automation);
            advanceSupervisor(std::chrono::milliseconds{99});
            require(unresponsive.load() == 0);
            advanceSupervisor(std::chrono::milliseconds{1});
            requireEventually([&] {
                return unresponsive.load() == 1;
            });
            require(automation->controller()->isClosed());
            require(discovery::list(sessions).empty());
            require(!std::filesystem::exists(automation->endpoint() / "endpoint.json"));
        });
    });
    given("a window that claims the session before the startup deadline", [&] {
        auto automation = enable(std::chrono::milliseconds{100}, std::chrono::seconds{1});
        automation->controller()->poll();
        then("an idle session is not revoked by the startup deadline", [&] {
            advanceSupervisor(std::chrono::milliseconds{1500});
            require(!automation->controller()->isClosed());
            require(unresponsive.load() == 0);
        });
    });
    given("the window closing: its connection is lost and show() returns within the grace", [&] {
        auto automation = enable(std::chrono::seconds{30});
        automation->controller()->close("Developer window disconnected");
        then("the record and the endpoint are withdrawn at once, and closing the automation confirms the window is gone", [&] {
            requireEventually([&] {
                return discovery::list(sessions).empty();
            });
            // Windows refuses to look at a file whose removal is pending: that is the withdrawal under way, not a failure.
            requireEventually([&] {
                std::error_code pending;
                return !std::filesystem::exists(automation->endpoint() / "endpoint.json", pending);
            });
            automation->close();
            require(unresponsive.load() == 0);
        });
    });
    given("a lost connection that leaves the native window open", [&] {
        auto automation = enable(std::chrono::milliseconds{100});
        // A navigation or a broken WebSocket: window.close() cannot reach the page, and show() does not return.
        automation->controller()->close("Developer window disconnected");
        then("the withdrawal is immediate and the window is still declared unresponsive after the grace", [&] {
            awaitCloseGrace(automation);
            requireEventually([&] {
                return discovery::list(sessions).empty();
            });
            advanceSupervisor(std::chrono::milliseconds{100});
            requireEventually([&] {
                return unresponsive.load() == 1;
            });
        });
    });
}};

const Scenario defaultAutomation{"The default automation clock supervises a renderer that never becomes ready", [] {
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    ConsumerApp       app{.project = root.path() / "project"};
    std::atomic<bool> unresponsive{false};
    auto              automation = enableAutomation(app,
                                                    {.listen         = root.path() / "endpoint",
                                                     .discovery      = root.path() / "sessions",
                                                     .startupTimeout = std::chrono::seconds{1},
                                                     .closeGrace     = std::chrono::milliseconds{10},
                                                     .onUnresponsive =
                                                         [&] {
                                                unresponsive = true;
                                                         },
                                                     .announce = false});
    requireEventually([&] {
        return unresponsive.load();
    });
    require(automation->controller()->isClosed());
    require(discovery::list(root.path() / "sessions").empty());
}};

const Scenario startupDeadlineSaturates{"A startup timeout too large for the clock never expires", [] {
    using namespace std::chrono;
    const auto launched = steady_clock::now();
    given("an ordinary timeout", [&] {
        then("the deadline is the launch plus the timeout", [&] {
            check(startupDeadline(launched, seconds{30}) == launched + seconds{30});
        });
    });
    given("the largest representable timeout", [&] {
        then("the deadline saturates instead of overflowing into the past", [&] {
            check(startupDeadline(launched, seconds::max()) == steady_clock::time_point::max());
            check(startupDeadline(launched, seconds::max()) > launched);
        });
    });
}};
