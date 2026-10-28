/// @brief Interruptible bounded stdin reads for the optional local MCP executable.
#pragma once
#include "Json.hpp"

#include <stop_token>
#include <thread>
#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
#else
    #include <poll.h>
    #include <unistd.h>
#endif

namespace webfront::automation {
class StdioInput {
public:
    enum class Status { waiting, line, closed };
    // Never blocks waiting for a complete line. A browser close can therefore stop the worker.
    Status read(std::string& line) {
        line.clear();
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return Status::line;
        }
        char bytes[4096];
#ifdef _WIN32
        const auto handle    = GetStdHandle(STD_INPUT_HANDLE);
        DWORD      available = 0;
        if (GetFileType(handle) == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
                return Status::closed;
            if (!available)
                return Status::waiting;
        } else if (GetFileType(handle) != FILE_TYPE_DISK) {
            throw std::runtime_error("MCP stdin must be a pipe or redirected file");
        }
        DWORD count = 0;
        if (!ReadFile(handle, bytes, sizeof bytes, &count, nullptr) || !count)
            return finish(line);
#else
        pollfd     descriptor{STDIN_FILENO, POLLIN, 0};
        const auto status = ::poll(&descriptor, 1, 0);
        if (status < 0) {
            if (errno == EINTR)
                return Status::waiting;
            throw std::runtime_error("Cannot poll MCP stdin");
        }
        if (!status)
            return Status::waiting;
        const auto count = ::read(STDIN_FILENO, bytes, sizeof bytes);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN)
                return Status::waiting;
            throw std::runtime_error("Cannot read MCP stdin");
        }
        if (!count)
            return finish(line);
#endif
        buffer.append(bytes, static_cast<std::size_t>(count));
        const auto end = buffer.find('\n');
        if ((end == std::string::npos ? buffer.size() : end) > messageLimit)
            throw std::invalid_argument("MCP input line exceeds 60000 bytes");
        // Drain already read lines without another OS read.
        if (end != std::string::npos) {
            line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            return Status::line;
        }
        return Status::waiting;
    }

private:
    std::string buffer;
    Status      finish(std::string& line) {
        if (buffer.empty())
            return Status::closed;
        line = std::exchange(buffer, {});
        return Status::line;
    }
};
}  // namespace webfront::automation
