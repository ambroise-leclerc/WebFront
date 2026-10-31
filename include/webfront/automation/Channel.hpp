/// @brief Opt-in authenticated native loopback channel for sharing an already open developer window.
#pragma once
#include "Credentials.hpp"
#include "Session.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <list>
#include <thread>
#include <utility>

namespace webfront::automation {
namespace channel {
inline bool retryableSocketError(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return platform::retryablePosixError(code);
#endif
}
inline int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
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
#ifndef _WIN32
struct Descriptor {
    int value;
    ~Descriptor() {
        if (value >= 0)
            ::close(value);
    }
    Descriptor(const Descriptor&) = delete;
    explicit Descriptor(int descriptor) : value(descriptor) { }
};
inline std::string readEndpointFile(int directory) {
    Descriptor  file{::openat(directory, "endpoint.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info{};
    if (file.value < 0 || ::fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Attachment endpoint must be owned by this user with no group/other access");
    std::string            text;
    std::array<char, 4097> bytes{};
    while (text.size() <= 4096) {
        const auto count = ::read(file.value, bytes.data(), bytes.size() - text.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::runtime_error("Cannot read private attachment endpoint");
        if (count == 0)
            break;
        text.append(bytes.data(), static_cast<std::size_t>(count));
    }
    if (text.empty() || text.size() > 4096)
        throw std::runtime_error("Invalid attachment endpoint size");
    return text;
}
#endif
inline std::string readEndpoint(const std::filesystem::path& directory) {
#ifdef _WIN32
    struct FileHandle {
        HANDLE value;
        ~FileHandle() {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    // Deny directory rename/deletion while resolving and reading its endpoint.
    FileHandle                 parent{CreateFileW(directory.c_str(),
                                                  FILE_READ_ATTRIBUTES,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                  nullptr,
                                                  OPEN_EXISTING,
                                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                  nullptr)};
    BY_HANDLE_FILE_INFORMATION parentInfo{};
    if (parent.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(parent.value, &parentInfo)
        || !(parentInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Attachment directory cannot be a reparse point");
    const auto                 path = directory / "endpoint.json";
    FileHandle                 file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nFileSizeHigh || info.nFileSizeLow > 4096)
        throw std::runtime_error("Cannot read bounded private attachment endpoint");
    std::array<char, 4097> bytes{};
    DWORD                  count = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count == 0 || count > 4096)
        throw std::runtime_error("Invalid attachment endpoint size");
    return {bytes.data(), static_cast<std::size_t>(count)};
#else
    Descriptor  parent{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info{};
    if (parent.value < 0 || ::fstat(parent.value, &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Attachment directory must be private and owned by this user");
    return readEndpointFile(parent.value);
#endif
}
inline bool waitUntil(Handle socket, short events, std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return remaining.count() > 0 && wait(socket, events, static_cast<int>(std::min(remaining, std::chrono::milliseconds{20}).count()));
}
struct SocketWrite {
    auto operator()(Handle socket, std::string_view bytes) const {
#ifdef _WIN32
        return ::send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
    #ifdef MSG_NOSIGNAL
        return ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    #else
        return ::send(socket, bytes.data(), bytes.size(), 0);
    #endif
#endif
    }
};
struct SocketRead {
    auto operator()(Handle socket, char* bytes, std::size_t size) const {
#ifdef _WIN32
        return ::recv(socket, bytes, static_cast<int>(size), 0);
#else
        return ::recv(socket, bytes, size, 0);
#endif
    }
};
template <typename Write = SocketWrite>
inline void send(Handle                                socket,
                 std::string_view                      bytes,
                 std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds{2},
                 Write                                 write = {}) {
    while (!bytes.empty() && std::chrono::steady_clock::now() < until) {
        if (!waitUntil(socket, POLLOUT, until))
            continue;
        const auto written = write(socket, bytes);
        if (written < 0 && retryableSocketError(socketError()))
            continue;
        if (written <= 0)
            throw TransportError("Native attachment send failed");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    if (!bytes.empty())
        throw TransportError("Native attachment send deadline exceeded");
}
template <typename Read = SocketRead>
inline std::string receive(Handle                                socket,
                           std::size_t                           limit,
                           std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds{2},
                           Read                                  read  = {}) {
    std::string text;
    while (std::chrono::steady_clock::now() < until) {
        if (!waitUntil(socket, POLLIN, until))
            continue;
        char       bytes[4096];
        const auto count = read(socket, bytes, sizeof bytes);
        if (count < 0 && retryableSocketError(socketError()))
            continue;
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
            try {
                serve(stop);
            } catch (const std::exception&) {
                listener.reset();
                privateDirectory.cleanup();
                session->close("Native attachment server failed");
            }
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
    // Unauthenticated peers own only bounded buffers/descriptors. They never occupy RPC threads.
    struct Connection {
        std::atomic<bool> finished{false};
        std::jthread      thread;
    };
    struct Pending {
        channel::Socket                       socket;
        std::string                           input;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        explicit Pending(channel::Handle handle) : socket(handle) {
            channel::nonblocking(handle);
        }
    };
    static constexpr std::size_t maxConnections = 32, maxPending = 64;
    static void                  refuse(channel::Handle socket, bool busy = false) {
        // A fresh peer has an empty send buffer. Never wait on an unauthenticated client.
        const auto message = busy ? std::string_view{"{\"error\":\"Native owner is busy\",\"code\":\"busy\"}\n"}
                                  : std::string_view{"{\"error\":\"Native attachment refused\",\"code\":\"not_authorized\"}\n"};
        (void)channel::SocketWrite{}(socket, message);
    }
    void serve(std::stop_token stop) {
        std::list<Connection> connections;
        std::list<Pending>    pending;
        struct Revoke {
            ChannelServer& owner;
            ~Revoke() {
                owner.listener.reset();
                owner.privateDirectory.cleanup();
            }
        } revoke{*this};
        while (!stop.stop_requested() && !session->isClosed()) {
            std::erase_if(connections, [](const Connection& connection) {
                return connection.finished.load();
            });
#ifdef _WIN32
            std::vector<WSAPOLLFD> items;
#else
            std::vector<pollfd> items;
#endif
            items.push_back({listener.value, POLLIN, 0});
            for (const auto& peer : pending)
                items.push_back({peer.socket.value, POLLIN, 0});
#ifdef _WIN32
            if (::WSAPoll(items.data(), static_cast<ULONG>(items.size()), 20) < 0)
                continue;
#else
            if (::poll(items.data(), static_cast<nfds_t>(items.size()), 20) < 0)
                continue;
#endif
            std::size_t index = 1;
            for (auto peer = pending.begin(); peer != pending.end();) {
                bool keep = std::chrono::steady_clock::now() < peer->deadline;
                if (keep && items[index].revents) {
                    char       bytes[4096];
                    const auto count = channel::SocketRead{}(peer->socket.value, bytes, sizeof bytes);
                    if (count > 0) {
                        peer->input.append(bytes, static_cast<std::size_t>(count));
                        const auto newline = peer->input.find('\n');
                        if ((newline == std::string::npos ? peer->input.size() : newline) > messageLimit)
                            keep = false;
                        else if (newline != std::string::npos) {
                            peer->input.resize(newline);
                            try {
                                const Object request{peer->input};
                                request.only({"version", "token", "method", "params"});
                                if (request.raw("version") != "1" || !channel::sameToken(request.string("token"), token))
                                    throw std::runtime_error("Unauthorized native attachment");
                                if (connections.size() >= maxConnections)
                                    refuse(peer->socket.value, true);
                                else {
                                    auto&           connection = connections.emplace_back();
                                    channel::Socket transfer{std::exchange(peer->socket.value, channel::invalid)};
                                    try {
                                        connection.thread = std::jthread([this, handle = transfer.value, input = std::move(peer->input), &connection] {
                                            handleConnection(handle, input);
                                            connection.finished.store(true);
                                        });
                                        transfer.value    = channel::invalid;
                                    } catch (...) {
                                        connection.finished.store(true);
                                        throw;
                                    }
                                }
                            } catch (const std::exception&) {
                                refuse(peer->socket.value);
                            }
                            keep = false;
                        }
                    } else if (count == 0 || !channel::retryableSocketError(channel::socketError()))
                        keep = false;
                }
                ++index;
                if (keep)
                    ++peer;
                else
                    peer = pending.erase(peer);
            }
            // Drain a bounded batch, then service readable peers before accepting another batch.
            if (items.front().revents)
                for (std::size_t accepted = 0; accepted < maxPending; ++accepted) {
                    const auto handle = channel::acceptSocket(listener.value);
                    if (handle == channel::invalid)
                        break;
                    if (pending.size() >= maxPending)
                        pending.pop_front();
                    pending.emplace_back(handle);
                }
        }
    }
    void handleConnection(channel::Handle handle, const std::string& input) {
        channel::Socket peer{handle};
        bool            authorized = false;
        try {
            channel::nonblocking(peer.value);
            const Object request{input};
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
    explicit ChannelClient(const std::filesystem::path& directory, std::chrono::milliseconds timeout = std::chrono::seconds{30}) : requestTimeout(timeout) {
        if (timeout <= std::chrono::milliseconds{0} || timeout > std::chrono::hours{1})
            throw std::invalid_argument("Invalid attachment request timeout");
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
    std::string describe(std::chrono::steady_clock::time_point deadline) const {
        try {
            return request("describe", "{}", deadline);
        } catch (const TransportError&) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw OperationError("timeout", "Developer startup timed out");
            throw;
        }
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
    std::chrono::milliseconds         requestTimeout;
    std::string                       request(std::string_view                      method,
                                              std::string_view                      arguments,
                                              std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) const {
        const auto      until = std::min(deadline, std::chrono::steady_clock::now() + requestTimeout);
        channel::Socket peer{channel::createSocket()};
        const auto      address = channel::address(port);
        // Loopback connect has no remote address or DNS lookup. Bound the following IO independently.
        channel::nonblocking(peer.value);
        const auto connected = ::connect(peer.value, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || (connected != 0 && !channel::wait(peer.value, POLLOUT, static_cast<int>(remaining))))
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
                          + "\n",
                      until);
        const Object response{channel::receive(peer.value, 256000, until), 256000};
        if (response.has("error"))
            throw OperationError(response.string("code", "not_authorized"), response.string("error"));
        return response.raw("result");
    }
};
}  // namespace webfront::automation
