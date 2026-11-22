// An application that opts in to automation (#501, ADR-025): its own window, its own `cppFunction` callbacks, and the MCP
// URL it serves itself. The browser regressions (AutomationClient.py, AutomationSdkClient.py) launch it and drive it from
// another process through that URL only; nothing here is a fake of the application the agent drives. It is a test
// program, not a shipped executable: WebFront has none, the application is the MCP server.
//
//   automation_host --project <dir> --environment <archive> --integrity <sha512-...> [--registry-dir <dir>]
//                   [--manifest-dir <dir>] --sessions <dir> [--directory <new private dir>] [--grants a,b,...]
//                   [--callbacks a,b,...] [--called <file>] [--timeout <seconds>] [--startup-timeout <seconds>]
//
// With --called, the application registers `consumer.render` (which appends its argument to the file) and
// `consumer.secret` (never allow-listed by the tests). Closing its standard input closes the window, on every platform and
// whatever the engine does with signals. The status is 0 after a normal closing, 70 when the developer environment failed
// to start (its reason on stderr), 71 when the native deadline of an operation revoked the session; a window that does not
// close after a deadline ends the process with enableAutomation's own status 72.
#include "../driver/WfDriver.hpp"
#include <webfront/automation/Enable.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#ifndef _WIN32
    #include <cerrno>
    #include <poll.h>
    #include <unistd.h>
#endif

namespace {
struct Pages {
    explicit Pages(const std::filesystem::path&) {}
    std::optional<webfront::fs::File> open(std::filesystem::path) const {
        return {};
    }
};

// Whether standard input reached its end, without blocking: the test closes it to close the window.
bool inputClosed() {
#ifdef _WIN32
    const HANDLE input     = GetStdHandle(STD_INPUT_HANDLE);
    DWORD        available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE;
    if (available > 0) {
        char  bytes[256];
        DWORD read = 0;
        ReadFile(input, bytes, std::min<DWORD>(available, sizeof bytes), &read, nullptr);
    }
    return false;
#else
    pollfd readable{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (::poll(&readable, 1, 0) <= 0)
        return false;
    char       bytes[256];
    const auto count = ::read(STDIN_FILENO, bytes, sizeof bytes);
    return count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN);
#endif
}

std::vector<std::string> split(const std::string& list) {
    std::vector<std::string> items;
    std::istringstream       input{list};
    for (std::string item; std::getline(input, item, ',');)
        if (!item.empty())
            items.push_back(item);
    return items;
}
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
        std::filesystem::path project, environment, registry, directory, sessions, called;
        std::string           integrity, manifest, grants = "check,build,preview,inspect,interact,call", callbacks = "consumer.render";
        std::chrono::seconds  timeout{30}, startupTimeout{90};
        unsigned short mcpPort = 0;
        bool stableMcp = false;
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
            else if (name == "--manifest-dir")
                manifest = value;
            else if (name == "--directory")
                directory = value;
            else if (name == "--sessions")
                sessions = value;
            else if (name == "--mcp-port")
                mcpPort = static_cast<unsigned short>(std::stoul(value));
            else if (name == "--stable-mcp")
                stableMcp = value == "true";
            else if (name == "--called")
                called = value;
            else if (name == "--grants")
                grants = value;
            else if (name == "--callbacks")
                callbacks = value;
            else if (name == "--timeout")
                timeout = std::chrono::seconds{std::stoi(value)};
            else if (name == "--startup-timeout")
                startupTimeout = std::chrono::seconds{std::stoi(value)};
            else {
                std::cerr << "Unknown option " << name << '\n';
                return 64;
            }
        }
        if (project.empty() || environment.empty() || integrity.empty() || sessions.empty() || (argc % 2) == 0) {
            std::cerr << "Usage: automation_host --project <dir> --environment <archive> --integrity <sha512> [--registry-dir <dir>] "
                         "[--manifest-dir <dir>] --sessions <dir> [--directory <dir>] [--grants a,b] [--callbacks a,b] [--called <file>] "
                         "[--timeout <seconds>] [--startup-timeout <seconds>]\n";
            return 64;
        }
        webfront::testdriver::sources() = {std::filesystem::canonical(environment),
                                           registry.empty() ? std::nullopt : std::optional{std::filesystem::canonical(registry)}};
        using App = webfront::BasicDeveloperApp<webfront::testdriver::DriverFrontend<Frontend>, webfront::testdriver::DriverFS, Pages>;
        App app{project, {.port = "0", .remoteFileTransport = webfront::RemoteFileTransport::loopbackAttested}};
        if (!called.empty()) {
            // The application's own callback: what a preview in this window can reach and a separate process cannot.
            app.cppFunction("consumer.render", [called](std::string text) {
                std::ofstream{called, std::ios::app} << text << '\n';
                return "rendered:" + text;
            });
            // Not allow-listed: an agent holding the call grant still cannot reach it.
            app.cppFunction("consumer.secret", [called] {
                std::ofstream{called, std::ios::app} << "SECRET\n";
                return std::string{"secret"};
            });
        }
        app.bootstrap("developer",
                      {.environment       = "/driver/environment.tgz",
                       .integrity         = integrity,
                       .registry          = registry.empty() ? std::nullopt : std::optional<std::string>{"/driver/registry/"},
                       .manifestDirectory = manifest});
        auto automation = webfront::automation::enableAutomation(app,
                                                                 {.grants         = split(grants),
                                                                  .callbacks      = called.empty() ? std::vector<std::string>{} : split(callbacks),
                                                                  .mcpPort        = mcpPort,
                                                                  .mcpToken       = stableMcp ? std::optional<std::string>{std::string(64, 'a')} : std::nullopt,
                                                                  .directory      = directory,
                                                                  .discovery      = sessions,
                                                                  .timeout        = timeout,
                                                                  .startupTimeout = startupTimeout});
        // The end of standard input closes the window natively, whatever the page does.
        std::jthread closer([&](std::stop_token shown) {
            while (!shown.stop_requested() && !inputClosed())
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            if (!shown.stop_requested())
                webfront::testdriver::closeUntilShown(shown, [&] { return app.requestClose(); }, std::chrono::seconds{10});
        });
        app.show(automation->launchPage());
        closer.request_stop();
        closer.join();
        const webfront::automation::Object state{automation->controller()->describe()};
        const auto                        failure = state.string("failure");
        int                               status  = 0;
        if (failure == "Native automation deadline exceeded")
            status = 71;
        else if (!automation->controller()->wasReady() && !failure.empty()) {
            std::cerr << "Automation startup: " << failure << '\n';
            status = 70;
        }
        automation->close();
        app.shutdown();
        return status;
    } catch (const std::exception& error) {
        std::cerr << "automation_host: " << error.what() << '\n';
        return 1;
    }
}
