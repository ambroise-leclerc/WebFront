/// @date 28/01/2022 16:51:42
/// @author Ambroise Leclerc
/// @brief Logging facilities
#pragma once

#if defined(WEBFRONT_USE_MDDLOG) && WEBFRONT_USE_MDDLOG
#include "MddlogLogger.hpp" // IWYU pragma: export
#else
#include "../details/C++20Support.hpp" // Provides <format> and <source_location>
#include "HexDump.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <string_view>

namespace webfront::log {
using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
const auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };
inline bool logTypeEnabled[Debug + 1];
inline struct Sinks {
    void operator()(std::string_view t) const {
        for (auto& s : sinks)
            if (s) s(t);
    }
    inline static std::vector<std::function<void(std::string_view)>> sinks;
} out;


// NOTE : functions below are defined inline static in order to avoir clang "uneeded function" and "unused function" errors
namespace {
    using namespace std;
    using srcLoc = source_location;
    
    constexpr auto toChar(LogType l) { return l == Debug ? 'D' : l == Warn ? 'W' : l == Error ? 'E' : 'I'; }
    inline static void log(LogType l, string_view text) { out(format("[{}] {:%T} | {}", toChar(l), chrono::system_clock::now(), text));}
    inline static void log(LogType l, string_view text, const srcLoc& s) {
        out(format("[{}] {:%T} | {:16}:{:4} | {}", toChar(l), chrono::system_clock::now(), filesystem::path(s.file_name()).filename().string(), s.line(), text));
    }
    template<typename... Ts> static void log(LogType l, string_view fmt, Ts&&... ts) {
#ifdef __cpp_lib_format
        log(l, vformat(fmt, make_format_args(ts...)));
#else
        log(l, format(fmt, std::forward<Ts>(ts)...));
#endif
    }
    template<typename... Ts> static void log(LogType l, string_view fmt, const srcLoc& s, Ts&&... ts) {
#if __cpp_lib_format
        log(l, vformat(fmt, make_format_args(ts...)), s);
#else
        log(l, format(fmt, std::forward<Ts>(ts)...), s);
#endif
    }

    inline static void set(LogType logType, bool enabled) { logTypeEnabled[logType] = enabled; }
    inline static bool is(LogType logType) { return logTypeEnabled[logType]; }
    inline static void setLogLevel(LogType l) { set(Error, l >= Error); set(Warn, l >= Warn); set(Info, l >= Info); set(Debug, l >= Debug);}
}

template<typename... Ts> struct debug { debug(string_view fmt, Ts&&... ts, const srcLoc& l = srcLoc::current()) {
    if (is(Debug)) log(Debug, fmt, l, std::forward<Ts>(ts)...);
    }
};
template<typename... Ts> debug(string_view, Ts&&...) -> debug<Ts...>;
template<typename... Ts> void error(string_view fmt, Ts&&... ts) { if (is(Error)) log(Error, fmt, std::forward<Ts>(ts)...); }
template<typename... Ts> void warn(string_view fmt, Ts&&... ts) { if (is(Warn)) log(Warn, fmt, std::forward<Ts>(ts)...); }
template<typename... Ts> void info(string_view fmt, Ts&&... ts) { if (is(Info)) log(Info, fmt, std::forward<Ts>(ts)...); }
void infoHex(string_view text, auto container) { if (is(Info)) { log(Info, text); out(utils::hexDump(container)); }}
auto addSinks(auto&&... ts) { (out.sinks.push_back(std::forward<decltype(ts)>(ts)), ...); return out.sinks.size() - 1; }
void removeSinks(auto&&... sinkIds) { ((out.sinks[sinkIds] = nullptr), ...); }

// Without mddlog no context is captured; the scope keeps call sites identical in both builds.
enum class CallDirection : uint8_t { None, CppToJs, JsToCpp };
struct Context { string_view component; string_view webLinkId; CallDirection direction = CallDirection::None; string_view callId; };
struct ContextScope {
    explicit ContextScope(Context) noexcept {}
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};

// Without mddlog a transport is a synchronous text sink: records carry only the rendered line.
struct TransportRecord { string_view text; };
struct TransportHandle { size_t index = numeric_limits<size_t>::max(); };
inline TransportHandle addTransport(function<void(const TransportRecord&)> write) {
    return {addSinks([write = std::move(write)](string_view t) { write(TransportRecord{t}); })};
}
inline void removeTransport(TransportHandle handle) { if (handle.index < out.sinks.size()) out.sinks[handle.index] = nullptr; }
inline void reportTransportFailure(TransportHandle handle) { removeTransport(handle); }
} //namespace webfront::log
#endif
