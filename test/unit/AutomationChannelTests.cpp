#include "TemporaryDirectory.hpp"
#include "Bdd.hpp"
#include <webfront/automation/Channel.hpp>

#include <deque>

using namespace webfront::bdd;
#ifndef _WIN32
    #include <spawn.h>
    #include <sys/wait.h>
extern char** environ;
#endif

#ifndef _WIN32
Scenario privateAutomation{"Private automation directories are born owner-only under a permissive umask", [] {
    webfront::test::TemporaryDirectory root;
    struct RestoreMask {
        mode_t previous = ::umask(0);
        ~RestoreMask() {
            ::umask(previous);
        }
    } restore;
    webfront::automation::channel::PrivateDirectory directory{root.path() / "profile"};
    struct stat                                     info{};
    require(::stat(directory.path.c_str(), &info) == 0);
    require((info.st_mode & 0777) == 0700);
}};
#endif

Scenario nativeAttachment{"Native attachment sockets cannot be inherited by renderer executables", [] {
    using namespace webfront::automation;
    [[maybe_unused]] channel::Runtime runtime;
    const auto                        check = [](channel::Handle socket) {
#ifdef _WIN32
        DWORD flags = HANDLE_FLAG_INHERIT;
        require(GetHandleInformation(reinterpret_cast<HANDLE>(socket), &flags));
        require((flags & HANDLE_FLAG_INHERIT) == 0);
#else
        require((::fcntl(socket, F_GETFD) & FD_CLOEXEC) != 0);
        // An actual exec must not expose a socket via the inherited descriptor table.
        const auto           command = "test ! -S /dev/fd/" + std::to_string(socket);
        std::array<char*, 4> args{const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command.c_str()), nullptr};
        pid_t                child = 0;
        require(::posix_spawn(&child, "/bin/sh", nullptr, nullptr, args.data(), environ) == 0);
        int status = 0;
        require(::waitpid(child, &status, 0) == child);
        require(WIFEXITED(status));
        require(WEXITSTATUS(status) == 0);
#endif
    };
    given("a listener, its client, an accepted peer, and an adopted socket", [&] {
        channel::Socket listener{channel::createSocket()};
        auto            address = channel::address(0);
        require(::bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        require(::listen(listener.value, 1) == 0);
#ifdef _WIN32
        int size = sizeof address;
#else
        socklen_t size = sizeof address;
#endif
        require(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        channel::Socket client{channel::createSocket()};
        require(::connect(client.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        channel::Socket accepted{channel::acceptSocket(listener.value)};
        channel::Socket adopted{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        for (const auto socket : {listener.value, client.value, accepted.value, adopted.value})
            check(socket);
        const auto transient = [](int index) {
#ifdef _WIN32
            WSASetLastError(index == 0 ? WSAEINTR : WSAEWOULDBLOCK);
#else
            constexpr std::array errors{EINTR, EAGAIN, EWOULDBLOCK};
            errno = errors[static_cast<std::size_t>(index)];
#endif
        };
        int        writes = 0, reads = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        channel::send(client.value, "hello\n", until, [&](channel::Handle socket, std::string_view bytes) -> std::ptrdiff_t {
            if (writes++ < 3) {
                transient(writes - 1);
                return -1;
            }
            return channel::SocketWrite{}(socket, bytes.substr(0, 2));
        });
        require(channel::receive(accepted.value,
                                 100,
                                 until,
                                 [&](channel::Handle socket, char* bytes, std::size_t capacity) -> std::ptrdiff_t {
                                     if (reads++ < 3) {
                                         transient(reads - 1);
                                         return -1;
                                     }
                                     return channel::SocketRead{}(socket, bytes, std::min(capacity, std::size_t{2}));
                                 })
                == "hello");
        require(writes >= 6);
        require(reads >= 6);
        requireThrows<TransportError>([&] { channel::send(client.value,
                                        "never",
                                        std::chrono::steady_clock::now() + std::chrono::milliseconds{2},
                                        [&](channel::Handle, std::string_view) -> int {
                                            transient(0);
                                            return -1;
                                        }); });
        accepted.reset();
        requireThrows<TransportError>([&] { channel::receive(client.value, 100); });
    });
}};

Scenario privateNative{"A private native attachment grants only the already running controller's authority", [] {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory directory;
    const auto                         endpoint = directory.path() / "attachment";
    const auto                         project  = directory.path() / "project";
    std::filesystem::create_directory(project);
    auto controller = std::make_shared<Session>(SessionOptions{project, {"build"}});
    controller->poll();
    requireThrows([&] { ChannelServer(controller, project / "page-readable-capability"); });
    require(!std::filesystem::exists(project / "page-readable-capability"));
    {
        ChannelServer owner{controller, endpoint};
        ChannelClient attached{endpoint};
        require(Object(attached.describe()).string("session") == Object(controller->describe()).string("session"));
        const auto session = Object(attached.describe()).string("session");
        const auto request = [&](std::string_view operation) {
            return Object(objectJson({
                {  "session",   webfront::json::quote(session)},
                {"operation", webfront::json::quote(operation)}
            }));
        };
        requireThrows([&] { attached.start(request("exec")); });
        requireThrows([&] { attached.start(request("test")); });
        const auto   run = Object(attached.start(request("build"))).string("runId");
        const Object query{
            objectJson({{"session", webfront::json::quote(session)}, {"runId", webfront::json::quote(run)}}
            )
        };
        attached.cancel(query);
        controller->poll();
        requireThrows([&] { ChannelServer(controller, project / "page-readable-capability"); });
        require(!std::filesystem::exists(project / "page-readable-capability"));
        require(Object(attached.result(query)).string("state") == "cancelled");
        requireThrows([&] { ChannelServer(controller, endpoint); });
        attached.shutdown();
        require(controller->isClosed());
        owner.stop();
        require(!std::filesystem::exists(endpoint));
        requireThrows<TransportError>([&] { attached.describe(); });
        requireThrows<TransportError>([&] { (void)ChannelClient(endpoint); });
    }
    require(!std::filesystem::exists(endpoint));
}};

Scenario nativeAttachmentCapabilities{"Native attachment capabilities are generated by the OS and never inferred from a session ID", [] {
    using namespace webfront::automation;
    const auto first  = channel::nonce();
    const auto second = channel::nonce();
    require(first.size() == 64);
    require(first != second);
    require(channel::sameToken(first, first));
    require(!channel::sameToken(first, second));
    require(!channel::sameToken(first, first.substr(1)));
}};

Scenario silentLoopback{"A silent loopback connection does not delay the owner's requests", [] {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory directory;
    const auto                         project = directory.path() / "project";
    std::filesystem::create_directory(project);
    auto controller = std::make_shared<Session>(SessionOptions{project, {"build"}});
    controller->poll();
    ChannelServer owner{controller, directory.path() / "attachment"};
    ChannelClient attached{directory.path() / "attachment"};
    const Object  endpoint{channel::readEndpoint(directory.path() / "attachment")};
    given("several peers that connect and send nothing", [&] {
        [[maybe_unused]] channel::Runtime runtime;
        std::deque<channel::Socket>       idle;  // Socket is neither copyable nor movable
        const auto                        port = static_cast<unsigned short>(std::stoi(endpoint.raw("port")));
        for (int index = 0; index < 80; ++index) {
            idle.emplace_back(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            const auto address = channel::address(port);
            require(::connect(idle.back().value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        }
        then("a legitimate request is answered well before the idle peers' 2 s receive deadline", [&] {
            const auto started = std::chrono::steady_clock::now();
            require(Object(attached.describe()).string("state") == "ready");
            require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{1500});
        });
    });
}};

#ifndef _WIN32
Scenario endpointReads{"Endpoint reads stay anchored to the private directory after a pathname swap", [] {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory root;
    const auto                         path = root.path() / "endpoint";
    channel::PrivateDirectory          original{path};
    channel::writeEndpoint(path / "endpoint.json", "original");
    channel::Descriptor pinned{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    require(pinned.value >= 0);
    const auto moved = root.path() / "moved";
    std::filesystem::rename(path, moved);
    channel::PrivateDirectory replacement{path};
    channel::writeEndpoint(path / "endpoint.json", "replacement");
    require(channel::readEndpointFile(pinned.value) == "original");
    require(channel::readEndpoint(path) == "replacement");
    std::filesystem::permissions(path, std::filesystem::perms::others_read, std::filesystem::perm_options::add);
    requireThrows([&] { channel::readEndpoint(path); });
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    replacement.cleanup();
    std::filesystem::create_directory_symlink(moved, path);
    requireThrows([&] { channel::readEndpoint(path); });
    std::filesystem::remove(path);
}};
#endif

Scenario attachmentResponse{"Attachment response deadlines are configurable independently of renderer work", [] {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory root;
    channel::PrivateDirectory          endpoint{root.path() / "endpoint"};
    [[maybe_unused]] channel::Runtime  runtime;
    channel::Socket                    listener{channel::createSocket()};
    auto                               address = channel::address(0);
    require(::bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
    require(::listen(listener.value, 2) == 0);
#ifdef _WIN32
    int length = sizeof address;
#else
    socklen_t length = sizeof address;
#endif
    require(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    channel::writeEndpoint(endpoint.path / "endpoint.json",
                           objectJson({
                               {"version",                                     "1"},
                               {   "port", std::to_string(ntohs(address.sin_port))},
                               {  "token", webfront::json::quote(channel::nonce())}
    }));
    std::jthread  server([&] {
        for (int index = 0; index < 2; ++index) {
            channel::Socket peer{channel::acceptSocket(listener.value)};
            try {
                (void)channel::receive(peer.value, 60000);
                std::this_thread::sleep_for(std::chrono::milliseconds{40});
                channel::send(peer.value, "{\"result\":{\"state\":\"ready\"}}\n");
            } catch (const TransportError&) { }
        }
    });
    ChannelClient impatient{endpoint.path, std::chrono::milliseconds{5}};
    requireThrows<TransportError>([&] { impatient.describe(); });
    ChannelClient patient{endpoint.path, std::chrono::milliseconds{5000}};
    require(Object(patient.describe()).string("state") == "ready");
}};
