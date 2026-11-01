// A tiny native program for the tests of webFrontHost and runProcess: one behaviour per first argument.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <process.h>
    #include <windows.h>
#else
    #include <csignal>

    #include <unistd.h>
extern char** environ;
#endif

namespace {
std::filesystem::path pathOf(const char* utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8)));
}

void sleepFor(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

// Starts this program again with the given mode and arguments, without waiting for it.
void startAgain(const char* self, const char* mode, const char* argument) {
#ifdef _WIN32
    _spawnl(_P_NOWAIT, self, self, mode, argument, nullptr);
#else
    if (fork() == 0) {
        execl(self, self, mode, argument, static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}
}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "args") {
        for (int index = 2; index < argc; ++index)
            std::cout << '[' << index - 2 << ']' << argv[index] << '\n';
        return 0;
    }
    if (mode == "exit")
        return argc > 2 ? std::atoi(argv[2]) : 0;
    if (mode == "streams") {
        std::cout << "to-stdout" << std::flush;
        std::cerr << "to-stderr" << std::flush;
        return 3;
    }
    if (mode == "big") {
        const auto        total = std::strtoull(argv[2], nullptr, 10);
        const std::string block(4096, 'x');
        for (unsigned long long written = 0; written < total; written += block.size())
            std::cout.write(block.data(), static_cast<std::streamsize>(std::min<unsigned long long>(block.size(), total - written)));
        return 0;
    }
    if (mode == "flood") {  // Never stops writing to either stream.
        const std::string block(65536, 'f');
        for (;;) {
            std::cout.write(block.data(), static_cast<std::streamsize>(block.size()));
            std::cerr.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    if (mode == "hang") {
        for (;;)
            sleepFor(100);
    }
    if (mode == "env") {
        const char* value = std::getenv(argv[2]);
        std::cout << (value ? value : "<unset>");
        return 0;
    }
    if (mode == "envcount") {
#ifdef _WIN32
        int count = 0;
        for (const wchar_t* entry = GetEnvironmentStringsW(); *entry; entry += wcslen(entry) + 1)
            ++count;
        std::cout << count;
#else
        int count = 0;
        for (char** entry = environ; *entry; ++entry)
            ++count;
        std::cout << count;
#endif
        return 0;
    }
    if (mode == "cwd") {
        std::cout << std::filesystem::current_path().generic_string();
        return 0;
    }
    if (mode == "cat") {
        std::ifstream in(pathOf(argv[2]), std::ios::binary);
        if (!in) {
            std::cerr << "cannot open " << argv[2];
            return 2;
        }
        std::cout << in.rdbuf();
        return 0;
    }
    if (mode == "write") {  // write FILE TEXT
        std::ofstream(pathOf(argv[2]), std::ios::binary) << argv[3];
        return 0;
    }
    if (mode == "stdin") {
        std::cout << std::distance(std::istreambuf_iterator<char>(std::cin), {});
        return 0;
    }
    if (mode == "write-later") {  // After two seconds: proves whether a descendant outlived a timeout.
        sleepFor(2000);
        std::ofstream(pathOf(argv[2])) << "alive";
        return 0;
    }
    if (mode == "spawn-later") {
        startAgain(argv[0], "write-later", argv[2]);
        for (;;)
            sleepFor(100);
    }
    if (mode == "spawn-quick") {  // Ends at once and leaves a descendant: it must not outlive the call either.
        startAgain(argv[0], "write-later", argv[2]);
        return 0;
    }
#ifndef _WIN32
    if (mode == "kill-self")
        raise(SIGKILL);
#endif
    std::cerr << "unknown mode\n";
    return 64;
}
