#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#else
    #include <csignal>

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
        return ::isatty(fd) ? 1 : 0;
    }
#endif
    return 3;
}
