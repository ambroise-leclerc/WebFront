/// @date 28/01/2022 16:51:42
/// @author Ambroise Leclerc
/// @brief Logging facilities
#pragma once
#include "../details/C++20Support.hpp" // Provides <format> and <source_location>
#include "HexDump.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <string_view>

namespace webfront::log {
using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
const auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };
inline bool logTypeEnabled[Debug + 1];
inline struct Sinks {
    void operator()(std::string_view t) const { for (auto& s : sinks) if (s) s(t); }
    inline static std::vector<std::function<void(std::string_view)>> sinks;
} out;

namespace {
    using namespace std;
    using srcLoc = source_location;
    
    constexpr auto toChar(LogType l) { return l == Debug ? 'D' : l == Warn ? 'W' : l == Error ? 'E' : 'I'; }
    void log(LogType l, string_view text) { out(format("[{}] {:%T} | {}", toChar(l), chrono::system_clock::now(), text));}
    void log(LogType l, string_view text, const srcLoc& s) {
        out(format("[{}] {:%T} | {:16}:{:4} | {}", toChar(l), chrono::system_clock::now(), filesystem::path(s.file_name()).filename().string(), s.line(), text));
    }
    template<typename... Ts> static void log(LogType l, string_view fmt, Ts&&... ts) {
#ifdef __cpp_lib_format
        log(l, vformat(fmt, make_format_args(forward<Ts>(ts)...)));
#else
        log(l, format(fmt, forward<Ts>(ts)...));
#endif
    }
    template<typename... Ts> static void log(LogType l, string_view fmt, const srcLoc& s, Ts&&... ts) {
#if __cpp_lib_format
        log(l, vformat(fmt, make_format_args(forward<Ts>(ts)...)), s);
#else
        log(l, format(fmt, forward<Ts>(ts)...), s);
#endif
    }

    void set(LogType logType, bool enabled) { logTypeEnabled[logType] = enabled; }
    bool is(LogType logType) { return logTypeEnabled[logType]; }
    void setLogLevel(LogType l) { set(Error, l >= Error); set(Warn, l >= Warn); set(Info, l >= Info); set(Debug, l >= Debug);}
}

template<typename... Ts> struct debug { debug(string_view fmt, Ts&&... ts, const srcLoc& l = srcLoc::current()) {
    if (is(Debug)) log(Debug, fmt, l, forward<Ts>(ts)...);
    }
};
template<typename... Ts> debug(string_view, Ts&&...) -> debug<Ts...>;
template<typename... Ts> void error(string_view fmt, Ts&&... ts) { if (is(Error)) log(Error, fmt, forward<Ts>(ts)...); }
template<typename... Ts> void warn(string_view fmt, Ts&&... ts) { if (is(Warn)) log(Warn, fmt, forward<Ts>(ts)...); }
template<typename... Ts> void info(string_view fmt, Ts&&... ts) { if (is(Info)) log(Info, fmt, forward<Ts>(ts)...); }
void infoHex(string_view text, auto container) { if (is(Info)) { log(Info, text); out(utils::hexDump(container)); }}
auto addSinks(auto&&... ts) { (out.sinks.push_back(forward<decltype(ts)>(ts)), ...); return out.sinks.size() - 1; }
void removeSinks(auto&&... sinkIds) { ((out.sinks[sinkIds] = nullptr), ...); }
} //namespace webfront::log