/// @brief Catchable native interrupts request orderly shutdown; handlers perform only a lock-free store.
#pragma once
#include "Platform.hpp"

#include <atomic>
#include <csignal>
#include <stdexcept>

namespace webfront::automation {
class Interrupt {
    inline static std::atomic<int> requestedSignal{0};
    static_assert(std::atomic<int>::is_always_lock_free);
    static void received(int number) noexcept {
        requestedSignal.store(number, std::memory_order_relaxed);
    }
#ifdef _WIN32
    using Handler = void (*)(int);
    Handler            previousInt, previousTerm;
    bool               consoleRegistered = false;
    static BOOL WINAPI console(DWORD event) {
        if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
            received(SIGINT);
            return TRUE;
        }
        return FALSE;
    }
#else
    struct sigaction previousInt{}, previousTerm{};
#endif
public:
    Interrupt() {
        requestedSignal.store(0, std::memory_order_relaxed);
#ifdef _WIN32
        previousInt       = std::signal(SIGINT, received);
        previousTerm      = std::signal(SIGTERM, received);
        consoleRegistered = SetConsoleCtrlHandler(console, TRUE) != 0;
#else
        struct sigaction action{};
        action.sa_handler = received;
        (void)sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, &previousInt))
            throw std::runtime_error("Cannot catch SIGINT");
        if (::sigaction(SIGTERM, &action, &previousTerm)) {
            ::sigaction(SIGINT, &previousInt, nullptr);
            throw std::runtime_error("Cannot catch SIGTERM");
        }
#endif
    }
    ~Interrupt() {
#ifdef _WIN32
        if (consoleRegistered)
            SetConsoleCtrlHandler(console, FALSE);
        if (previousInt != SIG_ERR)
            std::signal(SIGINT, previousInt);
        if (previousTerm != SIG_ERR)
            std::signal(SIGTERM, previousTerm);
#else
        ::sigaction(SIGINT, &previousInt, nullptr);
        ::sigaction(SIGTERM, &previousTerm, nullptr);
#endif
    }
    Interrupt(const Interrupt&)            = delete;
    Interrupt& operator=(const Interrupt&) = delete;
    int        signal() const noexcept {
        return requestedSignal.load(std::memory_order_relaxed);
    }
};
}  // namespace webfront::automation
