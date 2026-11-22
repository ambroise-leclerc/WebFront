/// @brief Native loopback sockets and private files of the MCP listener (McpHttp.hpp): non-inheritable sockets with bounded
/// reads and writes, a new owner-only directory outside the project, and files published whole and read without following a
/// swapped path.
#pragma once
#include "Credentials.hpp"
#include "Session.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
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
            throw std::runtime_error("Cannot initialize automation sockets");
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
        throw std::runtime_error("Cannot configure automation socket");
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
        throw std::runtime_error("Cannot configure automation socket");
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
            throw std::runtime_error("Cannot open automation socket");
        if (!nonInheritable(value)) {
            close(std::exchange(value, invalid));
            throw std::runtime_error("Cannot prevent automation socket inheritance");
        }
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
        const int enabled = 1;
        if (setsockopt(value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled)) {
            close(value);
            value = invalid;
            throw std::runtime_error("Cannot protect automation socket from SIGPIPE");
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
    // The developer bridge reads project files: the MCP token must live elsewhere.
    // Filesystem identity also handles case-insensitive Windows spellings of the same directory.
    for (auto parent = destination.parent_path(); !parent.empty();) {
        std::error_code error;
        if (std::filesystem::equivalent(project, parent, error))
            throw std::invalid_argument("The automation directory must be outside the developer project");
        const auto next = parent.parent_path();
        if (next == parent)
            break;
        parent = next;
    }
    return destination;
}

/// Whether a failed removal may succeed shortly: on Windows, a reader that has the record open (discovery `list` and `readEndpoint`
/// read files without sharing deletion) makes it fail with a sharing or lock violation, or with an access denial while a deletion is pending.
/// Nothing is transient on POSIX, where unlinking an open file succeeds.
inline bool transientRemovalFailure([[maybe_unused]] const std::error_code& error) noexcept {
#ifdef _WIN32
    return error.category() == std::system_category()
        && (error.value() == ERROR_SHARING_VIOLATION || error.value() == ERROR_LOCK_VIOLATION || error.value() == ERROR_ACCESS_DENIED);
#else
    return false;
#endif
}

/// Removes a file. A transient failure (see `transientRemovalFailure`) is retried until a one second deadline, because
/// giving up would leave a withdrawn session listed for as long as its process lives; any other failure is final.
inline void removeWithRetry(const std::filesystem::path& file) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    for (;;) {
        std::error_code error;
        std::filesystem::remove(file, error);
        if (!error || !transientRemovalFailure(error) || std::chrono::steady_clock::now() >= deadline)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

class PrivateDirectory {
public:
    explicit PrivateDirectory(std::filesystem::path requested) : path(std::filesystem::absolute(std::move(requested))) {
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("Cannot create private automation ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const auto          created = CreateDirectoryW(path.c_str(), &attributes);
        LocalFree(descriptor);
        if (!created)
            throw std::runtime_error("The automation directory must be new and private");
#else
        if (::mkdir(path.c_str(), 0700))
            throw std::runtime_error("The automation directory must be new and private");
#endif
    }
    ~PrivateDirectory() {
        cleanup();
    }
    /// Removes the known files, then the directory. Only the first call acts: after it the path is no longer this object's,
    /// and another instance may create the same directory again before this one is destroyed.
    void cleanup() noexcept {
        if (released.exchange(true))
            return;
        // The known files only: the MCP listener's client configuration may share the directory (McpHttp.hpp).
        for (const auto* name : {"endpoint.json", "endpoint.json.new", "mcp.json", "mcp.json.new"})
            removeWithRetry(path / name);
        removeWithRetry(path);
    }
    const std::filesystem::path path;

private:
    std::atomic<bool> released{false};
};
inline void writeEndpoint(const std::filesystem::path& path, std::string_view content) {
    auto staged = path;
    staged     += ".new";
#ifdef _WIN32
    const auto file = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot publish private automation file");
    DWORD      written = 0;
    const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) && written == content.size();
    CloseHandle(file);
#else
    const int file = ::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0)
        throw std::runtime_error("Cannot publish private automation file");
    const auto written = ::write(file, content.data(), content.size());
    const bool success = written >= 0 && static_cast<std::size_t>(written) == content.size();
    ::close(file);
#endif
    if (!success)
        throw std::runtime_error("Cannot finish publishing automation file");
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
inline std::string readEndpointFile(int directory, const char* name = "endpoint.json") {
    Descriptor  file{::openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info{};
    if (file.value < 0 || ::fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Automation file must be owned by this user with no group/other access");
    std::string            text;
    std::array<char, 4097> bytes{};
    while (text.size() <= 4096) {
        const auto count = ::read(file.value, bytes.data(), bytes.size() - text.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::runtime_error("Cannot read private automation file");
        if (count == 0)
            break;
        text.append(bytes.data(), static_cast<std::size_t>(count));
    }
    if (text.empty() || text.size() > 4096)
        throw std::runtime_error("Invalid automation file size");
    return text;
}
#endif
/// Reads `name` (a private file of at most 4096 bytes) from a private endpoint directory without following a swapped path.
inline std::string readEndpoint(const std::filesystem::path& directory, const char* name = "endpoint.json") {
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
        throw std::runtime_error("Automation directory cannot be a reparse point");
    const auto                 path = directory / name;
    FileHandle                 file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nFileSizeHigh || info.nFileSizeLow > 4096)
        throw std::runtime_error("Cannot read bounded private automation file");
    std::array<char, 4097> bytes{};
    DWORD                  count = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count == 0 || count > 4096)
        throw std::runtime_error("Invalid automation file size");
    return {bytes.data(), static_cast<std::size_t>(count)};
#else
    Descriptor  parent{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info{};
    if (parent.value < 0 || ::fstat(parent.value, &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Automation directory must be private and owned by this user");
    return readEndpointFile(parent.value, name);
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
            throw TransportError("Automation socket send failed");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    if (!bytes.empty())
        throw TransportError("Automation socket send deadline exceeded");
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
            throw TransportError("Automation socket connection closed");
        text.append(bytes, static_cast<std::size_t>(count));
        const auto end = text.find('\n');
        if ((end == std::string::npos ? text.size() : end) > limit)
            throw TransportError("Automation socket message exceeds its limit");
        if (end != std::string::npos) {
            text.resize(end);
            return text;
        }
    }
    throw TransportError("Automation socket receive deadline exceeded");
}
inline sockaddr_in address(unsigned short port) {
    sockaddr_in value{};
    value.sin_family      = AF_INET;
    value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    value.sin_port        = htons(port);
    return value;
}
}  // namespace channel

}  // namespace webfront::automation
