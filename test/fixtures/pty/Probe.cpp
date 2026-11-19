#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#else
    #include <csignal>

    #include <fcntl.h>
    #include <termios.h>
    #include <unistd.h>
#endif

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "stall") {
#ifdef _WIN32
        const auto pid = ::GetCurrentProcessId();
#else
        const auto pid = ::getpid();
        std::signal(SIGHUP, SIG_IGN);
        termios settings{};
        if (::tcgetattr(STDIN_FILENO, &settings) != 0)
            return 1;
        ::cfmakeraw(&settings);
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0)
            return 1;
#endif
        std::cout << "STALLED:" << pid << ":END\n" << std::flush;
        std::this_thread::sleep_for(30s);
        return 2;
    }
    if (mode == "size") {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info))
            return 1;
        std::cout << "SIZE:" << info.srWindow.Right - info.srWindow.Left + 1 << 'x' << info.srWindow.Bottom - info.srWindow.Top + 1 << '\n';
#else
        return 1;  // Unix coverage uses stty.
#endif
        return 0;
    }
    if (mode == "interrupt") {
        std::cout << "INTERRUPT_READY\n" << std::flush;
        std::this_thread::sleep_for(30s);
        return 2;
    }
#ifndef _WIN32
    if (mode == "fd" && argc == 3) {
        const auto fd = std::atoi(argv[2]);
        return ::fcntl(fd, F_GETFD) < 0 && errno == EBADF ? 0 : 1;
    }
    if (mode == "flood") {
        termios settings{};
        if (::tcgetattr(STDOUT_FILENO, &settings) != 0)
            return 1;
        ::cfmakeraw(&settings);
        if (::tcsetattr(STDOUT_FILENO, TCSANOW, &settings) != 0)
            return 2;
        const auto* environment = std::getenv("WF_TEST_ENVIRONMENT");
        if (::access("marker", F_OK) != 0 || !environment || std::string_view(environment) != "explicit")
            return 3;
        std::array<char, 4096> bytes{};
        for (std::size_t left = 1200000; left;) {
            const auto count = ::write(STDOUT_FILENO, bytes.data(), left < bytes.size() ? left : bytes.size());
            if (count <= 0)
                return 4;
            left -= static_cast<std::size_t>(count);
        }
        return ::write(STDOUT_FILENO, "COMPLETE", 8) == 8 ? 0 : 5;
    }
    if (mode == "reopen") {
        const char* name = ::ttyname(STDIN_FILENO);
        if (!name)
            return 1;
        const std::string path(name);
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
        std::this_thread::sleep_for(250ms);
        const auto slave = ::open(path.c_str(), O_WRONLY | O_NOCTTY);
        return slave >= 0 && ::write(slave, "REOPENED", 8) == 8 ? 0 : 2;
    }
    if (mode == "orphan") {
        int ready[2]{};
        if (::pipe(ready) < 0)
            return 1;
        const auto child = ::fork();
        if (child == 0) {
            ::close(ready[0]);
            if (::setpgid(0, 0) < 0)
                ::_exit(2);
            std::signal(SIGHUP, SIG_IGN);
            if (::write(ready[1], "!", 1) != 1)
                ::_exit(3);
            ::close(ready[1]);
            ::sleep(30);
            ::_exit(4);
        }
        ::close(ready[1]);
        char byte = 0;
        if (child < 0 || ::read(ready[0], &byte, 1) != 1 || ::tcsetpgrp(STDIN_FILENO, child) < 0)
            return 5;
        std::cout << "STALLED:" << child << ":END" << std::flush;
        return 0;
    }
#endif
    return 3;
}
