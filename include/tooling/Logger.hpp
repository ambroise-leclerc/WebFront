/// @date 28/01/2022 16:51:42
/// @author Ambroise Leclerc
/// @brief Logging facilities
#pragma once
#include "../details/C++20Support.hpp"
#include "HexDump.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <source_location>
#include <string_view>

namespace webfront {
namespace log {

using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
static auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };

namespace {
    using namespace std; using srcLoc = source_location;
    inline static struct Sinks {
        void operator()(string_view t) const { for (auto& s : sinks) if (s) s(t); }
        vector<function<void(string_view)>> sinks;
    } out;
    inline static std::array<bool, 5> logTypeEnabled;
    constexpr auto toChar(LogType l) { return l == Debug ? 'D' : l == Warn ? 'W' : l == Error ? 'E' : 'I'; }
    inline static void log(LogType l, string_view text) { out(format(std::string("[{}] {:%T} | {}"), toChar(l), chrono::system_clock::now(), text));}
    inline static void log(LogType l, string_view text, const srcLoc& s) {
        out(format(std::string("[{}] {:%T} | {:16}:{:4} | {}"), toChar(l), chrono::system_clock::now(), filesystem::path(s.file_name()).filename().string(), s.line(), text)); }
    inline static void set(LogType logType, bool enabled) { logTypeEnabled.at(logType) = enabled; }
    inline static bool is(LogType logType) { return logTypeEnabled.at(logType); }
    inline static void setLogLevel(LogType l) { set(Error, l >= Error); set(Warn, l >= Warn); set(Info, l >= Info); set(Debug, l >= Debug);}
}
template<typename... Ts> struct debug { debug(string_view fmt, Ts&&... ts, const srcLoc& l = srcLoc::current()) {if (is(Debug)) log(Debug, format(fmt, std::forward<Ts>(ts)...), l);}};
template<typename... Ts> debug(string_view, Ts&&...) -> debug<Ts...>;
template<typename... Ts> struct error { error(string_view fmt, Ts&&... ts) { if (is(Error)) log(Error, format(fmt, std::forward<Ts>(ts)...)); } };
template<typename... Ts> error(string_view, Ts&&...) -> error<Ts...>;
template<typename... Ts> struct warn { warn(string_view fmt, Ts&&... ts) { if (is(Warn)) log(Warn, format(fmt, std::forward<Ts>(ts)...)); } };
template<typename... Ts> warn(string_view, Ts&&...) -> warn<Ts...>;
template<typename... Ts> struct info { info(string_view fmt, Ts&&... ts) { if (is(Info)) log(Info, format(fmt, std::forward<Ts>(ts)...)); } };
template<typename... Ts> info(string_view, Ts&&...) -> info<Ts...>;
void infoHex(string_view text, auto container) { if (is(Info)) { log(Info, text); out(utils::hexDump(container)); }}
auto addSinks(auto&&... ts) { (out.sinks.push_back(forward<decltype(ts)>(ts)), ...); return out.sinks.size() - 1; }
void removeSinks(auto&&... sinkIds) { ((out.sinks[sinkIds] = nullptr), ...); }
} // namespace log
} // namespace webfront