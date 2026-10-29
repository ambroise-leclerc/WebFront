#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/automation/Channel.hpp>

#include <deque>
#ifndef _WIN32
    #include <spawn.h>
    #include <sys/wait.h>
extern char** environ;
#endif

#ifndef _WIN32
SCENARIO("Private automation directories are born owner-only under a permissive umask") {
    webfront::test::TemporaryDirectory root;
    struct RestoreMask {
        mode_t previous = ::umask(0);
        ~RestoreMask() {
            ::umask(previous);
        }
    } restore;
    webfront::automation::channel::PrivateDirectory directory{root.path() / "profile"};
    struct stat                                     info{};
    REQUIRE(::stat(directory.path.c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0700);
}
#endif

SCENARIO("Native attachment sockets cannot be inherited by renderer executables") {
    using namespace webfront::automation;
    [[maybe_unused]] channel::Runtime runtime;
    const auto                        check = [](channel::Handle socket) {
#ifdef _WIN32
        DWORD flags = HANDLE_FLAG_INHERIT;
        REQUIRE(GetHandleInformation(reinterpret_cast<HANDLE>(socket), &flags));
        REQUIRE((flags & HANDLE_FLAG_INHERIT) == 0);
#else
        REQUIRE((::fcntl(socket, F_GETFD) & FD_CLOEXEC) != 0);
        // An actual exec must not expose a socket via the inherited descriptor table.
        const auto           command = "test ! -S /dev/fd/" + std::to_string(socket);
        std::array<char*, 4> args{const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command.c_str()), nullptr};
        pid_t                child = 0;
        REQUIRE(::posix_spawn(&child, "/bin/sh", nullptr, nullptr, args.data(), environ) == 0);
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
#endif
    };
    GIVEN("a listener, its client, an accepted peer, and an adopted socket") {
        channel::Socket listener{channel::createSocket()};
        auto            address = channel::address(0);
        REQUIRE(::bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        REQUIRE(::listen(listener.value, 1) == 0);
#ifdef _WIN32
        int size = sizeof address;
#else
        socklen_t size = sizeof address;
#endif
        REQUIRE(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        channel::Socket client{channel::createSocket()};
        REQUIRE(::connect(client.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        channel::Socket accepted{channel::acceptSocket(listener.value)};
        channel::Socket adopted{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        for (const auto socket : {listener.value, client.value, accepted.value, adopted.value})
            check(socket);
        const auto transient = [](int index) {
#ifdef _WIN32
            WSASetLastError(index == 0 ? WSAEINTR : WSAEWOULDBLOCK);
#else
            errno = index == 0 ? EINTR : index == 1 ? EAGAIN : EWOULDBLOCK;
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
        REQUIRE(channel::receive(accepted.value,
                                 100,
                                 until,
                                 [&](channel::Handle socket, char* bytes, std::size_t size) -> std::ptrdiff_t {
                                     if (reads++ < 3) {
                                         transient(reads - 1);
                                         return -1;
                                     }
                                     return channel::SocketRead{}(socket, bytes, std::min(size, std::size_t{2}));
                                 })
                == "hello");
        REQUIRE(writes >= 6);
        REQUIRE(reads >= 6);
        REQUIRE_THROWS_AS(channel::send(client.value,
                                        "never",
                                        std::chrono::steady_clock::now() + std::chrono::milliseconds{2},
                                        [&](channel::Handle, std::string_view) -> int {
                                            transient(0);
                                            return -1;
                                        }),
                          TransportError);
        accepted.reset();
        REQUIRE_THROWS_AS(channel::receive(client.value, 100), TransportError);
    }
}

SCENARIO("A private native attachment grants only the already running controller's authority") {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory directory;
    const auto                         endpoint = directory.path() / "attachment";
    const auto                         project  = directory.path() / "project";
    std::filesystem::create_directory(project);
    auto controller = std::make_shared<Session>(SessionOptions{project, {"build"}});
    controller->poll();
    REQUIRE_THROWS(ChannelServer(controller, project / "page-readable-capability"));
    REQUIRE_FALSE(std::filesystem::exists(project / "page-readable-capability"));
    {
        ChannelServer owner{controller, endpoint};
        ChannelClient attached{endpoint};
        REQUIRE(Object(attached.describe()).string("session") == Object(controller->describe()).string("session"));
        const auto session = Object(attached.describe()).string("session");
        const auto request = [&](std::string_view operation) {
            return Object(objectJson({
                {  "session",   webfront::json::quote(session)},
                {"operation", webfront::json::quote(operation)}
            }));
        };
        REQUIRE_THROWS(attached.start(request("exec")));
        REQUIRE_THROWS(attached.start(request("test")));
        const auto   run = Object(attached.start(request("build"))).string("runId");
        const Object query{
            objectJson({{"session", webfront::json::quote(session)}, {"runId", webfront::json::quote(run)}}
            )
        };
        attached.cancel(query);
        controller->poll();
        REQUIRE_THROWS(ChannelServer(controller, project / "page-readable-capability"));
        REQUIRE_FALSE(std::filesystem::exists(project / "page-readable-capability"));
        REQUIRE(Object(attached.result(query)).string("state") == "cancelled");
        REQUIRE_THROWS(ChannelServer(controller, endpoint));
        attached.shutdown();
        REQUIRE(controller->isClosed());
        owner.stop();
        REQUIRE_FALSE(std::filesystem::exists(endpoint));
        REQUIRE_THROWS_AS(attached.describe(), TransportError);
        REQUIRE_THROWS_AS(ChannelClient(endpoint), TransportError);
    }
    REQUIRE_FALSE(std::filesystem::exists(endpoint));
}

SCENARIO("Native attachment capabilities are generated by the OS and never inferred from a session ID") {
    using namespace webfront::automation;
    const auto first  = channel::nonce();
    const auto second = channel::nonce();
    REQUIRE(first.size() == 64);
    REQUIRE(first != second);
    REQUIRE(channel::sameToken(first, first));
    REQUIRE_FALSE(channel::sameToken(first, second));
    REQUIRE_FALSE(channel::sameToken(first, first.substr(1)));
}

SCENARIO("A silent loopback connection does not delay the owner's requests") {
    using namespace webfront::automation;
    webfront::test::TemporaryDirectory directory;
    const auto                         project = directory.path() / "project";
    std::filesystem::create_directory(project);
    auto controller = std::make_shared<Session>(SessionOptions{project, {"build"}});
    controller->poll();
    ChannelServer owner{controller, directory.path() / "attachment"};
    ChannelClient attached{directory.path() / "attachment"};
    const Object  endpoint{channel::readEndpoint(directory.path() / "attachment")};
    GIVEN("several peers that connect and send nothing") {
        [[maybe_unused]] channel::Runtime runtime;
        std::deque<channel::Socket>       idle;  // Socket is neither copyable nor movable
        const auto                        port = static_cast<unsigned short>(std::stoi(endpoint.raw("port")));
        for (int index = 0; index < 4; ++index) {
            idle.emplace_back(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            const auto address = channel::address(port);
            REQUIRE(::connect(idle.back().value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        }
        THEN("a legitimate request is answered well before the idle peers' 2 s receive deadline") {
            const auto started = std::chrono::steady_clock::now();
            REQUIRE(Object(attached.describe()).string("state") == "ready");
            REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{1500});
        }
    }
}
