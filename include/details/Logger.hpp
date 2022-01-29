/// @file HexDump.hpp
/// @date 28/01/2022 16:51:42
/// @author Ambroise Leclerc
/// @brief Logging facilities
#pragma once
#include <details/C++20Support.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <string_view>

namespace webfront {
namespace log {

using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
static auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };

namespace {
    using namespace std;
    inline static struct Sinks {
        void operator()(string_view t) const { for (auto& s : sinks) s(t); }
        list<function<void(string_view)>> sinks;
    } out;
    inline static bool logTypeEnabled[5];
    constexpr char toChar(LogType l) { return l == Debug ? 'D' : l == Warn ? 'W' : l == Error ? 'E' : 'I'; }
    void log(LogType l, string_view text) { out(format("[{}] {:%T} | {}", toChar(l), chrono::system_clock::now(), text));}
    void log(LogType l, string_view text, const source_location& loc) {
        out(format("[{}] {:%T} | {:16}:{:4} | {}", toChar(l), chrono::system_clock::now(), filesystem::path(loc.file_name()).filename().string(), loc.line(), text)); }
    void set(LogType logType, bool enabled) { logTypeEnabled[logType] = enabled; }
    bool is(LogType logType) { return logTypeEnabled[logType]; }
    void setLogLevel(LogType l) { set(Error, l >= Error); set(Warn, l >= Warn); set(Info, l >= Info); set(Debug, l >= Debug);}
}

template <typename... Ts> struct debug {
    debug(string_view fmt, Ts&&... ts, const source_location& loc = source_location::current()) {if (is(Debug)) log(Debug, format(fmt, ts...), loc); }
};
template <typename... Ts> debug(string_view, Ts&&...) -> debug<Ts...>;
template <typename... Ts> struct error { error(string_view fmt, Ts&&... ts) { if (is(Error)) log(Error, format(fmt, ts...)); } };
template <typename... Ts> error(string_view, Ts&&...) -> error<Ts...>;
template <typename... Ts> struct warn { warn(string_view fmt, Ts&&... ts) { if (is(Warn)) log(Warn, format(fmt, ts...)); } };
template <typename... Ts> warn(string_view, Ts&&...) -> warn<Ts...>;
template <typename... Ts> struct info { info(string_view fmt, Ts&&... ts) { if (is(Info)) log(Info, format(fmt, ts...)); } };
template <typename... Ts> info(string_view, Ts&&...) -> info<Ts...>;
void infoHex(string_view text, auto container) { if (is(Info)) { log(Info, text); out(utils::hexDump(container)); }}

template <typename... Ts> void setSinks(Ts&&... ts) { (out.sinks.push_back(std::forward<Ts>(ts)), ...); }
} // namespace log
} // namespace webfront