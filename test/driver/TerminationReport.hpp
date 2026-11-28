// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-28
//
// WebFront — tests/driver
// Role: Says on standard error why a browser test program terminates abnormally, before the system ends it.
// Thread: worker-safe | Alloc: unrestricted
// LLM: test support; installed once per program by including WfDriver.hpp

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#ifdef _WIN32
    #include <stdlib.h>
#endif

namespace webfront::testdriver {

/// What std::terminate was called for: the active exception's message, or that there was none.
inline void reportTermination() noexcept {
    const auto exception = std::current_exception();
    if (!exception) {
        std::fputs("Test program terminated: std::terminate without an active exception\n", stderr);
        return;
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Test program terminated by an uncaught %s: %s\n", typeid(error).name(), error.what());
    } catch (...) {
        std::fputs("Test program terminated by an uncaught exception that is not a std::exception\n", stderr);
    }
}

/**
 * An exception that leaves main() or a thread, or a noexcept function, ends the program through std::terminate. Under
 * MSVC this is abort(), which fails fast with 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) and says nothing (#707): the
 * handler names the exception first. An invalid argument given to the C runtime (a closed descriptor, a null stream) also
 * ends a release build with that status, without a message: its handler says so before failing the same way.
 */
inline bool installTerminationReport() noexcept {
    std::set_terminate([] {
        reportTermination();
        std::fflush(stderr);
        std::abort();
    });
#ifdef _WIN32
    _set_invalid_parameter_handler([](const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line, std::uintptr_t) {
        // Release runtimes give no detail: the names are null there.
        std::fprintf(stderr,
                     "Test program terminated: invalid parameter given to the C runtime (%ls in %ls, %ls:%u)\n",
                     expression ? expression : L"?",
                     function ? function : L"?",
                     file ? file : L"?",
                     line);
        std::fflush(stderr);
        std::abort();
    });
#endif
    return true;
}

/// Installed before main() in every program that includes this header.
inline const bool terminationReported = installTerminationReport();

}  // namespace webfront::testdriver
