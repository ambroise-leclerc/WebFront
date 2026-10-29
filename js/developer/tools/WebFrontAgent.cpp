// Optional native launcher. Implementation stays in public header-only
// components.
#include <webfront/automation/Binding.hpp>
#include <webfront/automation/Channel.hpp>
#include <webfront/automation/Instructions.hpp>
#include <webfront/automation/Mcp.hpp>
#include <webfront/automation/Stdio.hpp>
#include <webfront/DeveloperApp.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace {
using namespace webfront;
using namespace webfront::automation;
struct Sources {
    std::filesystem::path archive, registry, profile;
};
Sources sources;

struct Files {
    explicit Files(const std::filesystem::path&) { }
    std::optional<fs::File> open(std::filesystem::path path) const {
        const auto            name = path.relative_path().generic_string();
        std::filesystem::path file;
        if (name == "automation/environment.tgz")
            file = sources.archive;
        else if (!sources.registry.empty() && name.starts_with("automation/registry/")) {
            try {
                file = developer::projectPath(sources.registry, std::string_view(name).substr(20));
            } catch (const std::exception&) {
                return {};
            }
        } else
            return {};
        std::ifstream input(file, std::ios::binary);
        if (input && std::filesystem::is_regular_file(file))
            return fs::File{std::move(input)};
        return {};
    }
};

struct Profile {
    channel::PrivateDirectory   directory{std::filesystem::temp_directory_path() / ("webfront-agent-" + channel::nonce())};
    const std::filesystem::path path{directory.path};
    ~Profile() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct WebViewOptions {
    static frontend::WebView2Options get() {
        frontend::WebView2Options options;
        options.userDataFolder = sources.profile;
        return options;
    }
};

struct CEFAgentFrontend : frontend::CEFFrontend {
    static void initialize() {
        webfront::detail::ensureFrontendInitialized<frontend::CEFFrontend>();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy) {
#if defined(WEBFRONT_EMBED_CEF) && defined(__linux__)
        webfront::cef::detail::openWindow(frontend::detail::applicationUrl(address, port, page),
                                          webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                          nullptr,
                                          sources.profile);
#else
        frontend::CEFFrontend::open(address, port, page, policy);
#endif
    }
};

static_assert(frontend::FrontendType<CEFAgentFrontend>);
using Selected = WEBFRONT_AUTOMATION_FRONTEND_TYPE;
using Engine   = std::conditional_t<
    std::is_same_v<Selected, frontend::CEFFrontend>,
    CEFAgentFrontend,
    std::conditional_t<std::is_same_v<Selected, frontend::WebView2Frontend>, frontend::WebView2FrontendWithOptions<WebViewOptions>, Selected>>;

struct Options {
    SessionOptions                     session;
    std::string                        integrity, manifestDirectory, operation;
    std::filesystem::path              listen;
    std::map<std::string, std::string> arguments;
    bool                               serve = false;
    std::chrono::seconds               startupTimeout{120};
};
std::string argumentName(std::string_view flag) {
    const auto& fields = argumentProperties();
    for (const auto& [key, schema] : fields.members()) {
        std::string option = "--";
        for (char c : key) {
            if (c >= 'A' && c <= 'Z') {
                option += '-';
                option += static_cast<char>(c - 'A' + 'a');
            } else
                option += c;
        }
        if (option == flag)
            return key;
    }
    return {};
}

int timeoutSeconds(std::string_view text) {
    int        seconds      = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds < 1 || seconds > 3600)
        throw std::invalid_argument("Timeout must be 1..3600 whole seconds");
    return seconds;
}

template <typename Backend>
int runCli(Backend&                                   controller,
           std::string_view                           operation,
           const std::map<std::string, std::string>& arguments,
           std::chrono::seconds                       startupTimeout,
           std::stop_token                            stop = {}) {
    const auto  deadline = std::chrono::steady_clock::now() + startupTimeout;
    auto        delay    = std::chrono::milliseconds{100};
    std::string identity;
    while (!stop.stop_requested()) {
        if (std::chrono::steady_clock::now() > deadline)
            throw OperationError("timeout", "Developer startup timed out");
        const Object snapshot{controller.describe()};
        if (snapshot.string("state") == "closed")
            throw OperationError("disconnected", "Developer session is closed");
        if (snapshot.string("state") != "starting") {
            identity = snapshot.string("session");
            break;
        }
        std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + delay));
        delay = std::min(delay * 2, std::chrono::milliseconds{1000});
    }
    if (stop.stop_requested())
        return 1;
    const Object request{
        objectJson({{"session", json::quote(identity)}, {"operation", json::quote(operation)}, {"arguments", objectJson(arguments)}}
        )
    };
    const auto   id = Object(controller.start(request)).string("runId");
    const Object query{
        objectJson({{"session", json::quote(identity)}, {"runId", json::quote(id)}}
        )
    };
    while (!stop.stop_requested()) {
        const auto result = controller.result(query);
        const auto state  = Object(result, 256000).string("state");
        if (state != "queued" && state != "running") {
            std::cout << result << '\n' << std::flush;
            return state == "succeeded" ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return 1;
}

template <typename Backend, typename Closed>
void runMcp(Backend& controller, std::stop_token stop, Closed&& closed) {
    Mcp         mcp{controller};
    StdioInput  input;
    std::string line;
    while (!stop.stop_requested() && !closed()) {
        const auto read = input.read(line);
        if (read == StdioInput::Status::closed)
            break;
        if (read == StdioInput::Status::line) {
            if (const auto reply = mcp.receive(line))
                std::cout << *reply << '\n' << std::flush;
            if (!std::cout)
                break;
        }
    }
}

int attach(int argc, char** argv) {
    if (argc < 3)
        throw std::invalid_argument("--attach needs the explicitly authorized endpoint directory");
    ChannelClient                      controller{argv[2]};
    std::string                        operation;
    std::map<std::string, std::string> arguments;
    bool                               shutdown = false;
    std::chrono::seconds               startupTimeout{120};
    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--mcp")
            continue;
        if (flag == "--shutdown") {
            shutdown = true;
            continue;
        }
        if (++index == argc)
            throw std::invalid_argument("Attachment argument needs a value");
        if (flag == "--operation")
            operation = argv[index];
        else if (flag == "--startup-timeout")
            startupTimeout = std::chrono::seconds{timeoutSeconds(argv[index])};
        else if (const auto name = argumentName(flag); !name.empty())
            arguments[name] = json::quote(argv[index]);
        else
            throw std::invalid_argument("Unknown attachment argument");
    }
    if (shutdown) {
        if (!operation.empty())
            throw std::invalid_argument("--shutdown cannot execute an operation");
        controller.shutdown();
        return 0;
    }
    if (!operation.empty())
        return runCli(controller, operation, arguments, startupTimeout);
    runMcp(controller, {}, [] {
        return false;
    });
    return 0;
}

Options parse(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto        value    = [&]() -> std::string {
            if (++i == argc)
                throw std::invalid_argument(argument + " needs a value");
            return argv[i];
        };
        if (argument == "--project")
            result.session.project = value();
        else if (argument == "--environment")
            sources.archive = value();
        else if (argument == "--integrity")
            result.integrity = value();
        else if (argument == "--registry-dir")
            sources.registry = value();
        else if (argument == "--manifest-dir")
            result.manifestDirectory = value();
        else if (argument == "--allow")
            result.session.allow.push_back(value());
        else if (argument == "--operation")
            result.operation = value();
        else if (argument == "--listen")
            result.listen = value();
        else if (argument == "--serve")
            result.serve = true;
        else if (const auto field = argumentName(argument); !field.empty())
            result.arguments[field] = json::quote(value());
        else if (argument == "--mcp") {
        } else if (argument == "--timeout" || argument == "--startup-timeout") {
            const int seconds = timeoutSeconds(value());
            if (argument == "--startup-timeout")
                result.startupTimeout = std::chrono::seconds{seconds};
            else
                result.session.timeout = std::chrono::seconds{seconds};
        } else
            throw std::invalid_argument("Unknown argument: " + argument);
    }
    if (result.session.project.empty() || sources.archive.empty() || !result.integrity.starts_with("sha512-"))
        throw std::invalid_argument("Use --project <directory> --environment <archive> --integrity "
                                    "<sha512-SRI> [--allow <operation>] [--mcp | --operation <name>]");
    sources.archive = std::filesystem::canonical(sources.archive);
    if (!std::filesystem::is_regular_file(sources.archive))
        throw std::invalid_argument("Environment must be a file");
    if (!sources.registry.empty())
        sources.registry = developer::canonicalDirectory(sources.registry);
    if (result.serve && (result.listen.empty() || !result.operation.empty()))
        throw std::invalid_argument("--serve requires --listen and cannot execute a CLI operation");
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--schema") {
            std::cout << contractSource << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--agent-help") {
            std::cout << agentInstructions;
            return 0;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--attach")
            return attach(argc, argv);
#ifndef _WIN32
        // A renderer surviving forced termination must not retain the agent's
        // CLI/MCP pipes: clients otherwise wait for EOF after the agent exits.
        for (int descriptor = 0; descriptor != 3; ++descriptor) {
            const auto flags = ::fcntl(descriptor, F_GETFD);
            if (flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
                throw std::runtime_error("Cannot isolate automation stdio from renderer subprocesses");
        }
#endif
        // Dispatch CEF renderer processes before parsing flags or creating a
        // profile.
        webfront::detail::ensureFrontendInitialized<Selected>();
        const auto options = parse(argc, argv);
        Profile    profile;
        sources.profile = profile.path;
        auto session    = std::make_shared<Session>(options.session);
        using App       = BasicDeveloperApp<Engine, Files>;
        App app{options.session.project, {.port = "0"}};
        app.bootstrap("developer",
                      {.environment       = "/automation/environment.tgz",
                       .integrity         = options.integrity,
                       .registry          = sources.registry.empty() ? std::nullopt : std::optional<std::string>{"/automation/registry/"},
                       .manifestDirectory = options.manifestDirectory});
        Binding                      binding{app, session};
        std::optional<ChannelServer> channel;
        if (!options.listen.empty())
            channel.emplace(session, options.listen);
        std::atomic<int>  status{0};
        std::atomic<bool> done{false};
        std::atomic<bool> finished{false};  // the transport completed its work: a closed session is then a normal ending
        const auto        launched = std::chrono::steady_clock::now();
        std::jthread      supervisor([&](std::stop_token stop) {
            while (!stop.stop_requested() && !done.load()) {
                const bool starting = !session->wasReady() && !session->isClosed();
                if (session->expire() || (starting && std::chrono::steady_clock::now() - launched > options.startupTimeout)) {
                    status.store(71);
                    session->close("Native automation deadline exceeded");
                }
                if (session->isClosed()) {
                    binding.closeBrowser();
                    // Neither a blocked renderer nor a blocked stdio writer may keep
                    // shutdown alive forever.
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    while (!stop.stop_requested() && !done.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    if (!stop.stop_requested() && !done.load()) {
                        // A normal ending keeps its own status; only an abnormal one is reported as stuck.
                        const int code = status.load() ? status.load() : finished.load() ? 0 : 72;
                        if (channel)
                            channel->stop();
                        std::error_code ignored;
                        std::filesystem::remove_all(profile.path, ignored);  // _Exit skips the destructors
                        std::_Exit(code);
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        });
        std::jthread      transport([&](std::stop_token stop) {
            try {
                if (options.serve) {
                    while (!stop.stop_requested() && !session->isClosed())
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                } else if (!options.operation.empty()) {
                    const auto result = runCli(*session, options.operation, options.arguments, options.startupTimeout, stop);
                    int        expected = 0;
                    status.compare_exchange_strong(expected, result);
                } else
                    runMcp(*session, stop, [&] {
                        return session->isClosed();
                    });
                finished.store(true);
            } catch (const std::exception& error) {
                std::cerr << "Automation: " << error.what() << '\n';
                int expected = 0;
                status.compare_exchange_strong(expected, 1);
            }
            session->close();
        });
        app.show(binding.launchPage());
        if (!session->wasReady() && !Object(session->describe()).string("failure").empty() && status.load() == 0)
            status.store(70);
        session->close("Developer window closed");
        transport.request_stop();
        app.shutdown();
        transport.join();
        done.store(true);
        supervisor.request_stop();
        return status.load();
    } catch (const TransportError& error) {
        std::cerr << "webfront-agent: " << error.what() << "; request outcome may be unknown\n";
        return 69;
    } catch (const OperationError& error) {
        std::cerr << "webfront-agent: " << error.what() << '\n';
        return error.code == "timeout" ? 71 : error.code == "disconnected" ? 69 : 1;
    } catch (const std::exception& error) {
        std::cerr << "webfront-agent: " << error.what() << '\n';
        return 64;
    }
}
