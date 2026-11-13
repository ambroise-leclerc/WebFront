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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront::log {
using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
const auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };
inline std::array<std::atomic<bool>, Debug + 1> logTypeEnabled{};
inline struct Sinks {
    void operator()(std::string_view t) const {
        // The registry lock only copies a pointer: the snapshot is rebuilt when a sink is added or removed, never per line.
        std::shared_ptr<const Snapshot> snapshot;
        {
            const std::lock_guard lock(mutex);
            snapshot = published;
        }
        // Never invoke a callback under the registry lock: it may log or change registrations.
        if (snapshot)
            for (const auto& entry : *snapshot)
                entry->invoke(t);
    }

    auto add(auto&&... callbacks) {
        const std::lock_guard lock(mutex);
        ((sinks.push_back(std::make_shared<Entry>(std::forward<decltype(callbacks)>(callbacks))), retired.emplace_back()), ...);
        publish();
        return sinks.size() - 1;
    }

    void remove(std::size_t index) {
        std::shared_ptr<Entry> entry;
        {
            const std::lock_guard lock(mutex);
            if (index >= sinks.size())
                return;
            entry = std::exchange(sinks[index], nullptr);
            if (entry) {
                retired[index] = entry;  // weak: it never keeps the callback's captured state alive
                publish();
            } else {
                // A second removal outside a callback still waits for a delivery that the first one is waiting for. The
                // weak reference is expired once no thread runs the callback: then there is nothing to wait for.
                entry = retired[index].lock();
            }
        }
        if (entry)
            entry->retire(dispatchDepth == 0);
    }

private:
    struct Entry;
    using Snapshot = std::vector<std::shared_ptr<Entry>>;
    // Removing from a callback cannot wait for callbacks on other logging threads: two callbacks can detach each
    // other. Outside delivery, removal is a quiescence barrier before the caller releases captured state.
    // Contract: never remove a sink while holding a lock that the sink's callback takes (the removal would wait for a
    // callback that waits for that lock). WebLink removes its transport after releasing transportMutex.
    inline static thread_local unsigned dispatchDepth = 0;
    struct Entry {
        explicit Entry(std::function<void(std::string_view)> function) : callback(std::move(function)) { }
        void invoke(std::string_view line) {
            {
                const std::lock_guard lock(mutex);
                if (!alive || !callback)
                    return;
                ++active;
            }
            struct Exit {
                Entry& entry;
                ~Exit() {
                    --dispatchDepth;
                    const std::lock_guard lock(entry.mutex);
                    --entry.active;
                    entry.idle.notify_all();
                }
            } exit{*this};
            ++dispatchDepth;
            callback(line);
        }
        void retire(bool wait) {
            std::unique_lock lock(mutex);
            alive = false;
            if (wait)
                idle.wait(lock, [&] {
                    return active == 0;
                });
        }
        std::mutex                                  mutex;
        std::condition_variable                     idle;
        bool                                        alive  = true;
        std::size_t                                 active = 0;
        const std::function<void(std::string_view)> callback;
    };
    // Called with the registry lock held.
    static void publish() {
        auto live = std::make_shared<Snapshot>();
        for (const auto& entry : sinks)
            if (entry)
                live->push_back(entry);
        published = std::move(live);
    }
    inline static std::mutex                        mutex;
    inline static std::vector<std::shared_ptr<Entry>> sinks;
    inline static std::vector<std::weak_ptr<Entry>>   retired;  // parallel to sinks
    inline static std::shared_ptr<const Snapshot>     published;
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

    inline static void set(LogType logType, bool enabled) { logTypeEnabled[logType].store(enabled, std::memory_order_relaxed); }
    inline static bool is(LogType logType) { return logTypeEnabled[logType].load(std::memory_order_relaxed); }
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
auto addSinks(auto&&... ts) { return out.add(std::forward<decltype(ts)>(ts)...); }
void removeSinks(auto&&... sinkIds) { (out.remove(sinkIds), ...); }

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
inline void removeTransport(TransportHandle handle) { out.remove(handle.index); }
inline void reportTransportFailure(TransportHandle handle) { removeTransport(handle); }
inline void reportTransportOverflow() noexcept {}
} //namespace webfront::log
#endif
