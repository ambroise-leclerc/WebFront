// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-23
//
// WebFront — frontend
// Role: The startup trace (WEBFRONT_STARTUP_TRACE=1) and the presentation of native windows shared by the native frontends.
// Thread: ui-loop | Alloc: heap-init
// LLM: diagnostic only; output goes to standard error, keep it free of side effects

// The startup trace (WEBFRONT_STARTUP_TRACE=1) and the presentation of the native windows, shared by the native
// frontends (#652).

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>

#if defined(_WIN32)
    #include "../system/WindowsCompat.hpp"

    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <sys/time.h>
    #include <unistd.h>
#elif defined(__linux__)
    #include <fstream>
    #include <sstream>
    #include <string>

    #include <unistd.h>
#endif

namespace webfront::frontend::detail {

/// Milliseconds since the process was executed, so that a trace covers the loading of the libraries and the toolkit's
/// initialization, not only the session; -1 when the platform does not tell.
[[nodiscard]] inline long long millisecondsSinceProcessStart() noexcept {
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER start{}, current{};
    start.LowPart    = creation.dwLowDateTime;
    start.HighPart   = creation.dwHighDateTime;
    current.LowPart  = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    return static_cast<long long>((current.QuadPart - start.QuadPart) / 10000);
#elif defined(__APPLE__)
    int         mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    kinfo_proc  info{};
    std::size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return -1;
    const timeval started = info.kp_proc.p_un.__p_starttime;
    timeval       now{};
    gettimeofday(&now, nullptr);
    return static_cast<long long>(now.tv_sec - started.tv_sec) * 1000 + (now.tv_usec - started.tv_usec) / 1000;
#elif defined(__linux__)
    try {  // field 22 of /proc/self/stat (clock ticks since boot) against the uptime
        std::ifstream stat("/proc/self/stat");
        std::string   text;
        std::getline(stat, text);
        const auto close = text.rfind(')');  // the command name may contain spaces
        if (close == std::string::npos)
            return -1;
        std::istringstream fields(text.substr(close + 2));
        std::string        field;
        for (int index = 3; index <= 22 && fields >> field; ++index)
            if (index == 22) {
                std::ifstream uptime("/proc/uptime");
                double        seconds = 0;
                uptime >> seconds;
                return static_cast<long long>((seconds - std::stod(field) / static_cast<double>(sysconf(_SC_CLK_TCK))) * 1000.0);
            }
    } catch (...) {}
    return -1;
#else
    return -1;
#endif
}

/// WEBFRONT_STARTUP_TRACE=1 reports the milestones of a view's start on standard error: one object per session, whose
/// creation is the origin of the session's own clock.
class StartupTrace {
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();

    /// Read at each milestone, not once: an application may set the variable between two openings. On Windows the process
    /// environment block is read, which SetEnvironmentVariableW changes and the C runtime's copy does not follow.
    [[nodiscard]] static bool enabled() noexcept {
#if defined(_WIN32)
        wchar_t     text[2] = {};
        const DWORD length  = GetEnvironmentVariableW(L"WEBFRONT_STARTUP_TRACE", text, 2);  // a longer value returns its size: not "0"
        return length > 0 && !(length == 1 && text[0] == L'0');
#else
        const auto* text = std::getenv("WEBFRONT_STARTUP_TRACE");
        return text && *text && std::string_view{text} != "0";
#endif
    }

public:
    void operator()(const char* milestone) const noexcept {
        if (enabled())
            std::fprintf(stderr,
                         "WebFront startup: %s at %lld ms (session %lld ms)\n",
                         milestone,
                         millisecondsSinceProcessStart(),
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - created).count()));
    }
};

/// The window appears nearly transparent and becomes opaque once, at the page's first frame or when the delay expires.
/// This decides; the frontend sets the opacity and runs the timer.
struct Presentation {
    /// The window is made opaque at the latest this long after the load starts, painted or not.
    static constexpr unsigned maximumDelayMs = 1000;

    bool                  shown   = false;
    bool                  byFrame = false;  // the page's first frame, not the delay, made the window opaque
    std::function<void()> windowShown;      // test hook: called once the window is made opaque

    /// True for the first call only: the caller then shows the window and calls announce().
    [[nodiscard]] bool begin(bool frame) noexcept {
        if (shown)
            return false;
        shown   = true;
        byFrame = frame;
        return true;
    }
    void announce() const {
        if (windowShown)
            windowShown();
    }
};

}  // namespace webfront::frontend::detail
