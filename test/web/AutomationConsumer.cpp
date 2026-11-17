// A consumer application that opts in to automation (#501): its own window and its own `cppFunction`, driven by
// `webfront-agent --attach`. The browser regression (AutomationClient.py, scenario `consumer`) launches it and attaches
// from another process; nothing here is a fake of the application the agent drives.
//
//   automation_consumer --project <dir> --environment <archive> --integrity <sha512-...> [--registry-dir <dir>]
//                       --listen <new private dir> --sessions <dir> --called <file>
#include "../driver/WfDriver.hpp"
#include <webfront/automation/Enable.hpp>

#include <fstream>
#include <iostream>

namespace {
struct Pages {
    explicit Pages(const std::filesystem::path&) {}
    std::optional<webfront::fs::File> open(std::filesystem::path) const {
        return {};
    }
};
}  // namespace

int main(int argc, char** argv) {
    using Frontend = WEBFRONT_DRIVER_FRONTEND;
    try {
        webfront::detail::ensureFrontendInitialized<Frontend>();
        [[maybe_unused]] const auto requestLog = webfront::testdriver::logRequests();
#if defined(WEBFRONT_EMBED_CEF) && defined(__linux__)
        std::optional<webfront::test::TemporaryDirectory> temporaryProfile;
        if constexpr (std::is_same_v<Frontend, webfront::frontend::CEFFrontend>) {
            if (!std::getenv("WEBFRONT_TEST_CEF_PROFILE")) {
                temporaryProfile.emplace();
                ::setenv("WEBFRONT_TEST_CEF_PROFILE", temporaryProfile->path().c_str(), 1);
            }
        }
#endif
        std::filesystem::path project, environment, registry, listen, sessions, called;
        std::string           integrity;
        for (int i = 1; i + 1 < argc; i += 2) {
            const std::string_view name{argv[i]};
            const std::string      value{argv[i + 1]};
            if (name == "--project")
                project = value;
            else if (name == "--environment")
                environment = value;
            else if (name == "--integrity")
                integrity = value;
            else if (name == "--registry-dir")
                registry = value;
            else if (name == "--listen")
                listen = value;
            else if (name == "--sessions")
                sessions = value;
            else if (name == "--called")
                called = value;
            else {
                std::cerr << "Unknown option " << name << '\n';
                return 64;
            }
        }
        if (project.empty() || environment.empty() || integrity.empty() || listen.empty() || sessions.empty() || called.empty()) {
            std::cerr << "Usage: automation_consumer --project <dir> --environment <archive> --integrity <sha512> [--registry-dir <dir>] "
                         "--listen <dir> --sessions <dir> --called <file>\n";
            return 64;
        }
        webfront::testdriver::sources() = {std::filesystem::canonical(environment),
                                           registry.empty() ? std::nullopt : std::optional{std::filesystem::canonical(registry)}};
        using App = webfront::BasicDeveloperApp<webfront::testdriver::DriverFrontend<Frontend>, webfront::testdriver::DriverFS, Pages>;
        App app{project, {.port = "0", .remoteFileTransport = webfront::RemoteFileTransport::loopbackAttested}};
        // The application's own callback: what a preview in this window can reach and a separate agent process cannot.
        app.cppFunction("consumer.render", [called](std::string text) {
            std::ofstream{called, std::ios::app} << text << '\n';
            return "rendered:" + text;
        });
        // Not allow-listed: an agent holding the call grant still cannot reach it.
        app.cppFunction("consumer.secret", [called] {
            std::ofstream{called, std::ios::app} << "SECRET\n";
            return std::string{"secret"};
        });
        app.bootstrap("developer",
                      {.environment = "/driver/environment.tgz",
                       .integrity   = integrity,
                       .registry    = registry.empty() ? std::nullopt : std::optional<std::string>{"/driver/registry/"}});
        auto automation = webfront::automation::enableAutomation(app,
                                                                 {.grants    = {"check", "build", "preview", "inspect", "interact", "call"},
                                                                  .callbacks = {"consumer.render"},
                                                                  .listen    = listen,
                                                                  .discovery = sessions,
                                                                  .timeout   = std::chrono::seconds{30},
                                                                  .startupTimeout = std::chrono::seconds{90}});
        app.show(automation->launchPage());
        automation->close();
        app.shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "automation_consumer: " << error.what() << '\n';
        return 1;
    }
}
