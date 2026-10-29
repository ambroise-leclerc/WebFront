/// @brief Opt-in authenticated native loopback channel for sharing an already open developer window.
#pragma once
#include "Session.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <list>
#include <thread>
#include <utility>
#ifdef _WIN32
    // WinSock must precede Windows, and BCrypt requires Windows' type declarations.
    #ifndef NOMINMAX
        #define NOMINMAX
        #define WEBFRONT_AUTOMATION_RESTORE_NOMINMAX
    #endif
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <sddl.h>
    // clang-format on
    #ifdef WEBFRONT_AUTOMATION_RESTORE_NOMINMAX
        #undef WEBFRONT_AUTOMATION_RESTORE_NOMINMAX
        #undef NOMINMAX
    #endif
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #if defined(__APPLE__) || defined(__linux__)
        #include <sys/random.h>
    #endif
#endif

namespace webfront::automation {
namespace channel {
#ifdef _WIN32
using Handle                    = SOCKET;
inline constexpr Handle invalid = INVALID_SOCKET;
inline void             close(Handle socket) {
    ::closesocket(socket);
}
struct Runtime {
    Runtime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("Cannot initialize native attachment sockets");
    }
    ~Runtime() {
        WSACleanup();
    }
};
inline bool wait(Handle socket, short events, int milliseconds) {
    WSAPOLLFD item{socket, events, 0};
    return WSAPoll(&item, 1, milliseconds) > 0;
}
inline void nonblocking(Handle socket) {
    u_long on = 1;
    if (ioctlsocket(socket, FIONBIO, &on))
        throw std::runtime_error("Cannot configure native attachment socket");
}
#else
using Handle                    = int;
inline constexpr Handle invalid = -1;
inline void             close(Handle socket) {
    ::close(socket);
}
struct Runtime { };
inline bool wait(Handle socket, short events, int milliseconds) {
    pollfd item{socket, events, 0};
    return ::poll(&item, 1, milliseconds) > 0;
}
inline void nonblocking(Handle socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK))
        throw std::runtime_error("Cannot configure native attachment socket");
}
#endif
inline bool nonInheritable(Handle socket) {
#ifdef _WIN32
    return SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0) != 0;
#else
    const auto flags = ::fcntl(socket, F_GETFD);
    return flags >= 0 && ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
}
inline Handle createSocket() {
#ifdef _WIN32
    const auto socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    const auto socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (socket != invalid && !nonInheritable(socket)) {
        close(socket);
        return invalid;
    }
    return socket;
}
inline Handle acceptSocket(Handle listener) {
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    const auto socket = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
    const auto socket = ::accept(listener, nullptr, nullptr);
#endif
    if (socket != invalid && !nonInheritable(socket)) {
        close(socket);
        return invalid;
    }
    return socket;
}
struct Socket {
    Handle value = invalid;
    explicit Socket(Handle handle) : value(handle) {
        if (value == invalid)
            throw std::runtime_error("Cannot open native attachment socket");
        if (!nonInheritable(value)) {
            close(std::exchange(value, invalid));
            throw std::runtime_error("Cannot prevent native attachment socket inheritance");
        }
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
        const int enabled = 1;
        if (setsockopt(value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled)) {
            close(value);
            value = invalid;
            throw std::runtime_error("Cannot protect native attachment socket from SIGPIPE");
        }
#endif
    }
    ~Socket() {
        reset();
    }
    void reset() noexcept {
        if (value != invalid)
            close(std::exchange(value, invalid));
    }
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;
};
inline std::string nonce() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot generate attachment capability");
#else
    if (getentropy(bytes.data(), bytes.size()))
        throw std::runtime_error("Cannot generate attachment capability");
#endif
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                result;
    for (auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
inline bool sameToken(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    unsigned mismatch = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        mismatch |= static_cast<unsigned>(static_cast<unsigned char>(left[index])) ^ static_cast<unsigned>(static_cast<unsigned char>(right[index]));
    return mismatch == 0;
}

inline std::filesystem::path outsideProject(const Session& controller, const std::filesystem::path& requested) {
    const auto project     = std::filesystem::path(Object(controller.describe()).string("project"));
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(requested));
    // The developer bridge reads project files: its attachment capability must live elsewhere.
    // Filesystem identity also handles case-insensitive Windows spellings of the same directory.
    for (auto parent = destination.parent_path(); !parent.empty();) {
        std::error_code error;
        if (std::filesystem::equivalent(project, parent, error))
            throw std::invalid_argument("Attachment directory must be outside the developer project");
        const auto next = parent.parent_path();
        if (next == parent)
            break;
        parent = next;
    }
    return destination;
}

class PrivateDirectory {
public:
    explicit PrivateDirectory(std::filesystem::path requested) : path(std::filesystem::absolute(std::move(requested))) {
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("Cannot create private attachment ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const auto          created = CreateDirectoryW(path.c_str(), &attributes);
        LocalFree(descriptor);
        if (!created)
            throw std::runtime_error("Attachment directory must be new and private");
#else
        if (::mkdir(path.c_str(), 0700))
            throw std::runtime_error("Attachment directory must be new and private");
#endif
    }
    ~PrivateDirectory() {
        cleanup();
    }
    void cleanup() const noexcept {
        std::error_code ignored;
        std::filesystem::remove(path / "endpoint.json", ignored);
        std::filesystem::remove(path / "endpoint.json.new", ignored);
        std::filesystem::remove(path, ignored);
    }
    const std::filesystem::path path;
};
inline void writeEndpoint(const std::filesystem::path& path, std::string_view content) {
    auto staged = path;
    staged     += ".new";
#ifdef _WIN32
    const auto file = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot publish private attachment endpoint");
    DWORD      written = 0;
    const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) && written == content.size();
    CloseHandle(file);
#else
    const int file = ::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0)
        throw std::runtime_error("Cannot publish private attachment endpoint");
    const auto written = ::write(file, content.data(), content.size());
    const bool success = written >= 0 && static_cast<std::size_t>(written) == content.size();
    ::close(file);
#endif
    if (!success)
        throw std::runtime_error("Cannot finish publishing attachment endpoint");
    // Publish only complete bytes, after closing the file; clients never observe a partial capability.
    std::filesystem::rename(staged, path);
}
inline std::string readEndpoint(const std::filesystem::path& directory) {
    const auto path = directory / "endpoint.json";
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)) || std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
        throw std::runtime_error("Attachment endpoint cannot be a symbolic link");
#ifdef _WIN32
    if ((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        throw std::runtime_error("Attachment endpoint cannot be a reparse point");
    std::ifstream input(path, std::ios::binary);
    std::string   text(4097, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    if (!input.eof() || text.size() > 4096)
        throw std::runtime_error("Cannot read bounded attachment endpoint");
    return text;
#else
    const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        throw std::runtime_error("Cannot open private attachment endpoint");
    struct stat info{};
    if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0) {
        ::close(file);
        throw std::runtime_error("Attachment endpoint must be owned by this user with no group/other access");
    }
    std::array<char, 4097> bytes{};
    const auto             count = ::read(file, bytes.data(), bytes.size());
    ::close(file);
    if (count < 1 || static_cast<std::size_t>(count) > 4096)
        throw std::runtime_error("Invalid attachment endpoint size");
    return {bytes.data(), static_cast<std::size_t>(count)};
#endif
}
inline void send(Handle socket, std::string_view bytes) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!bytes.empty() && std::chrono::steady_clock::now() < until) {
        if (!wait(socket, POLLOUT, 20))
            continue;
#ifdef _WIN32
        const auto written = ::send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
    #ifdef MSG_NOSIGNAL
        const auto written = ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    #else
        const auto written = ::send(socket, bytes.data(), bytes.size(), 0);
    #endif
#endif
        if (written <= 0)
            throw TransportError("Native attachment send failed");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    if (!bytes.empty())
        throw TransportError("Native attachment send deadline exceeded");
}
inline std::string receive(Handle socket, std::size_t limit) {
    const auto  until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::string text;
    while (std::chrono::steady_clock::now() < until) {
        if (!wait(socket, POLLIN, 20))
            continue;
        char       bytes[4096];
        const auto count = ::recv(socket, bytes, sizeof bytes, 0);
        if (count <= 0)
            throw TransportError("Native attachment connection closed");
        text.append(bytes, static_cast<std::size_t>(count));
        const auto end = text.find('\n');
        if ((end == std::string::npos ? text.size() : end) > limit)
            throw TransportError("Native attachment message exceeds its limit");
        if (end != std::string::npos) {
            text.resize(end);
            return text;
        }
    }
    throw TransportError("Native attachment receive deadline exceeded");
}
inline sockaddr_in address(unsigned short port) {
    sockaddr_in value{};
    value.sin_family      = AF_INET;
    value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    value.sin_port        = htons(port);
    return value;
}
}  // namespace channel

class ChannelServer {
public:
    ChannelServer(std::shared_ptr<Session> controller, const std::filesystem::path& directory)
        : session(std::move(controller)),
          privateDirectory(channel::outsideProject(*session, directory)),
          listener(channel::createSocket()),
          token(channel::nonce()) {
        auto address = channel::address(0);
        if (::bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) || ::listen(listener.value, 8))
            throw std::runtime_error("Cannot bind private loopback attachment endpoint");
#ifdef _WIN32
        int size = sizeof address;
#else
        socklen_t size = sizeof address;
#endif
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size))
            throw std::runtime_error("Cannot discover attachment port");
        channel::nonblocking(listener.value);
        channel::writeEndpoint(privateDirectory.path / "endpoint.json",
                               objectJson({
                                   {"version",                                     "1"},
                                   {   "port", std::to_string(ntohs(address.sin_port))},
                                   {  "token",                      json::quote(token)}
        }));
        worker = std::jthread([this](std::stop_token stop) {
            serve(stop);
        });
    }
    ~ChannelServer() {
        stop();
    }
    // Explicit revocation is also used before _Exit, which skips C++ destructors.
    void stop() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        privateDirectory.cleanup();
    }

private:
    [[maybe_unused]] channel::Runtime runtime;
    std::shared_ptr<Session>          session;
    channel::PrivateDirectory         privateDirectory;
    channel::Socket                   listener;
    std::string                       token;
    std::jthread                      worker;
    // One thread per connection, so a peer that connects and stays silent only holds its own slot until the receive
    // deadline; the owner's clients are served meanwhile. Excess connections are refused rather than queued.
    struct Connection {
        std::atomic<bool> finished{false};
        std::jthread      thread;
    };
    static constexpr std::size_t maxConnections = 32;
    void                         serve(std::stop_token stop) {
        std::list<Connection> connections;  // joined when this function returns, each bounded by the IO deadlines
        struct Revoke {
            ChannelServer& owner;
            ~Revoke() {
                owner.listener.reset();
                owner.privateDirectory.cleanup();
            }
        } revoke{*this};  // revoke the endpoint before joining pending connection handlers
        while (!stop.stop_requested() && !session->isClosed()) {
            std::erase_if(connections, [](const Connection& connection) {
                return connection.finished.load();
            });
            if (!channel::wait(listener.value, POLLIN, 20))
                continue;
            const auto handle = channel::acceptSocket(listener.value);
            if (handle == channel::invalid)
                continue;
            if (connections.size() >= maxConnections) {
                channel::Socket refused{handle};
                continue;
            }
            auto& connection  = connections.emplace_back();
            connection.thread = std::jthread([this, handle, &connection] {
                handleConnection(handle);
                connection.finished.store(true);
            });
        }
    }
    void handleConnection(channel::Handle handle) {
        channel::Socket peer{handle};
        bool            authorized = false;
        try {
            channel::nonblocking(peer.value);
            const Object request{channel::receive(peer.value, messageLimit)};
            request.only({"version", "token", "method", "params"});
            if (request.raw("version") != "1" || !channel::sameToken(request.string("token"), token))
                throw std::runtime_error("Unauthorized native attachment");
            authorized          = true;
            const auto   method = request.string("method");
            const Object arguments{request.raw("params", "{}")};
            std::string  result;
            if (method == "describe") {
                arguments.only({});
                result = session->describe();
            } else if (method == "start")
                result = session->start(arguments);
            else if (method == "result")
                result = session->result(arguments);
            else if (method == "cancel")
                result = session->cancel(arguments);
            else if (method == "shutdown") {
                arguments.only({});
                session->close("Closed by the authorized native owner");
                result = "{}";
            } else
                throw std::runtime_error("Unknown native attachment method");
            channel::send(peer.value, "{\"result\":" + result + "}\n");
        } catch (const std::exception& failure) {
            // Never echo tokens, request data, or implementation exceptions to an unauthenticated peer.
            try {
                const auto* operation = dynamic_cast<const OperationError*>(&failure);
                channel::send(peer.value,
                              objectJson({
                                  {"error",                           json::quote(authorized ? failure.what() : "Native attachment refused")},
                                  { "code", json::quote(authorized ? (operation ? operation->code : "invalid_arguments") : "not_authorized")}
                })
                                  + "\n");
            } catch (const std::exception&) { }
        }
    }
};

class ChannelClient {
public:
    explicit ChannelClient(const std::filesystem::path& directory) {
        std::error_code unavailable;
        if (!std::filesystem::is_regular_file(directory / "endpoint.json", unavailable))
            throw TransportError("Native attachment endpoint is unavailable");
        const Object endpoint{channel::readEndpoint(directory)};
        endpoint.only({"version", "port", "token"});
        if (endpoint.raw("version") != "1")
            throw std::runtime_error("Incompatible native attachment endpoint");
        const auto text         = endpoint.raw("port");
        unsigned   parsed       = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size() || parsed < 1 || parsed > 65535)
            throw std::runtime_error("Invalid native attachment port");
        port  = static_cast<unsigned short>(parsed);
        token = endpoint.string("token");
        if (token.size() != 64)
            throw std::runtime_error("Invalid native attachment capability");
    }
    std::string describe() const {
        return request("describe", "{}");
    }
    std::string start(const Object& arguments) const {
        return request("start", objectJson(arguments.members()));
    }
    std::string result(const Object& arguments) const {
        return request("result", objectJson(arguments.members()));
    }
    std::string cancel(const Object& arguments) const {
        return request("cancel", objectJson(arguments.members()));
    }
    void shutdown() const {
        request("shutdown", "{}");
    }

private:
    [[maybe_unused]] channel::Runtime runtime;
    unsigned short                    port = 0;
    std::string                       token;
    std::string                       request(std::string_view method, std::string_view arguments) const {
        channel::Socket peer{channel::createSocket()};
        const auto      address = channel::address(port);
        // Loopback connect has no remote address or DNS lookup. Bound the following IO independently.
        channel::nonblocking(peer.value);
        const auto connected = ::connect(peer.value, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        if (connected != 0 && !channel::wait(peer.value, POLLOUT, 2000))
            throw TransportError("Native attachment connect deadline exceeded");
        int status = 0;
#ifdef _WIN32
        int        length  = sizeof status;
        const auto checked = getsockopt(peer.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &length);
#else
        socklen_t  length  = sizeof status;
        const auto checked = getsockopt(peer.value, SOL_SOCKET, SO_ERROR, &status, &length);
#endif
        if (checked || status)
            throw TransportError("Native attachment is unavailable");
        channel::send(peer.value,
                      objectJson({
                          {"version",                    "1"},
                          {  "token",     json::quote(token)},
                          { "method",    json::quote(method)},
                          { "params", std::string(arguments)}
        })
                          + "\n");
        const Object response{channel::receive(peer.value, 256000), 256000};
        if (response.has("error"))
            throw OperationError(response.string("code", "not_authorized"), response.string("error"));
        return response.raw("result");
    }
};
}  // namespace webfront::automation
