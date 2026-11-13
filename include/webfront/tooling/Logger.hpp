/// @date 28/01/2022 16:51:42
/// @author Ambroise Leclerc
/// @brief Logging facilities
#pragma once

#if defined(WEBFRONT_USE_MDDLOG) && WEBFRONT_USE_MDDLOG
#include "MddlogLogger.hpp" // IWYU pragma: export
#else
#include "../details/C++20Support.hpp" // Provides <format> and <source_location>
#include "HexDump.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::log {
using LogType = const uint8_t;
constinit LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
const auto clogSink = [](std::string_view t) { std::clog << t << "\n"; };
inline std::array<std::atomic<bool>, Debug + 1> logTypeEnabled{};
/// Identifies one registered sink. A slot is reused once its sink is gone, so the generation makes a stale id harmless:
/// removing it again can never reach the sink that took the slot over.
struct SinkId {
    std::size_t index      = std::numeric_limits<std::size_t>::max();
    std::size_t generation = 0;
    friend bool operator==(const SinkId&, const SinkId&) = default;
};
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

    /// One callback gives its id, several give an array of ids, as the module logger does.
    template <typename... Callbacks>
    auto add(Callbacks&&... callbacks) {
        if constexpr (sizeof...(Callbacks) == 1)
            return (addOne(std::forward<Callbacks>(callbacks)), ...);
        else
            return std::array<SinkId, sizeof...(Callbacks)>{addOne(std::forward<Callbacks>(callbacks))...};
    }

    /// Returns once no other thread runs the sink's callback, so the caller may release what it captured. Called from
    /// inside a callback it also waits for the sink's deliveries on other threads, unless two callbacks would then wait
    /// for each other: that wait is given up (see Entry::awaitQuiescence), because it could never end.
    void remove(SinkId id) {
        std::shared_ptr<Entry> entry;
        {
            const std::lock_guard lock(mutex);
            if (id.index >= slots.size() || slots[id.index].generation != id.generation)
                return;  // never added, or a slot that a later sink took over
            auto& slot = slots[id.index];
            entry      = std::exchange(slot.entry, nullptr);
            if (entry) {
                slot.retired = entry;  // the entry drops its callback as soon as no thread runs it (Entry::releaseIfQuiet)
                publish();
            } else {
                // A second removal still waits for a delivery that the first one is waiting for; once no thread runs the
                // callback there is nothing to wait for.
                entry = slot.retired;
            }
        }
        if (entry)
            entry->retire();
    }

private:
    struct Entry;
    using Snapshot = std::vector<std::shared_ptr<Entry>>;
    struct Slot {
        std::shared_ptr<Entry> entry;
        std::shared_ptr<Entry> retired;  // strong, but its callback is released once nothing runs it
        std::size_t            generation = 0;
    };
    // The callbacks being run by this thread, innermost last.
    inline static thread_local std::vector<const Entry*> dispatchStack;
    // Contract: never remove a sink while holding a lock that the sink's callback takes (the removal would wait for a
    // callback that waits for that lock). WebLink removes its transport after releasing transportMutex.
    struct Entry : std::enable_shared_from_this<Entry> {
        explicit Entry(std::function<void(std::string_view)> function) : callback(std::move(function)) { }
        void invoke(std::string_view line) {
            {
                const std::lock_guard lock(mutex);
                if (!alive || !callback)
                    return;
                runners.push_back(std::this_thread::get_id());
            }
            dispatchStack.push_back(this);
            struct Exit {
                Entry& entry;
                ~Exit() {
                    dispatchStack.pop_back();
                    std::function<void(std::string_view)> released;  // destroyed after the lock is dropped
                    const std::lock_guard lock(entry.mutex);
                    entry.runners.erase(std::find(entry.runners.begin(), entry.runners.end(), std::this_thread::get_id()));
                    entry.idle.notify_all();
                    released = entry.takeCallbackIfQuiet();
                }
            } exit{*this};
            callback(line);
        }
        void retire() {
            const auto self = std::this_thread::get_id();
            std::function<void(std::string_view)> released;  // destroyed after the lock is dropped
            {
                const std::lock_guard lock(mutex);
                alive    = false;
                released = takeCallbackIfQuiet();
            }
            // This thread's own deliveries of the sink (it removes itself) cannot end before this call returns.
            WaitRegistry::Waiting waiting{self, this->weak_from_this()};
            std::unique_lock      lock(mutex);
            while (std::count_if(runners.begin(), runners.end(), [&](auto id) { return id != self; }) != 0) {
                lock.unlock();
                const bool cycle = WaitRegistry::wouldWaitForever(self, *this);
                lock.lock();
                if (cycle)
                    return;
                idle.wait_for(lock, std::chrono::milliseconds{5});
            }
        }
        // Once the sink is removed and no thread runs it, the callback and what it captured are no longer needed: giving
        // them up here lets a retired slot be kept without keeping them alive. Called with `mutex` held; the caller
        // destroys the result after releasing it.
        std::function<void(std::string_view)> takeCallbackIfQuiet() {
            if (alive || !runners.empty())
                return {};
            return std::move(callback);
        }
        bool quiet() {
            const std::lock_guard lock(mutex);
            return !alive && runners.empty();
        }
        std::vector<std::thread::id> runnersOtherThan(std::thread::id self) {
            const std::lock_guard lock(mutex);
            std::vector<std::thread::id> others;
            for (const auto id : runners)
                if (id != self)
                    others.push_back(id);
            return others;
        }
        std::mutex                                  mutex;
        std::condition_variable                     idle;
        bool                                        alive = true;
        std::vector<std::thread::id>                runners;  // one item per callback running now
        std::function<void(std::string_view)>       callback;
    };
    // Which sink each thread is waiting to see finish. A removal that would wait for a thread that is itself (directly or
    // through others) waiting for a callback of the remover is a deadlock, and gives up instead.
    struct WaitRegistry {
        struct Waiting {
            Waiting(std::thread::id thread, std::weak_ptr<Entry> entry) : id(thread) {
                const std::lock_guard lock(mutex());
                waits()[id] = std::move(entry);
            }
            ~Waiting() {
                const std::lock_guard lock(mutex());
                waits().erase(id);
            }
            std::thread::id id;
        };
        static bool wouldWaitForever(std::thread::id self, Entry& target) {
            std::vector<std::thread::id> pending = target.runnersOtherThan(self);
            std::vector<std::thread::id> seen;
            while (!pending.empty()) {
                const auto thread = pending.back();
                pending.pop_back();
                if (std::find(seen.begin(), seen.end(), thread) != seen.end())
                    continue;
                seen.push_back(thread);
                // A strong reference, taken under the registry lock: the waiting thread may finish and release the entry
                // while this one still reads it.
                std::shared_ptr<Entry> waitedFor;
                {
                    const std::lock_guard lock(mutex());
                    if (const auto found = waits().find(thread); found != waits().end())
                        waitedFor = found->second.lock();
                }
                if (!waitedFor)
                    continue;  // running, not waiting: it will finish its callback
                for (const auto next : waitedFor->runnersOtherThan(thread)) {
                    if (next == self)
                        return true;
                    pending.push_back(next);
                }
            }
            return false;
        }
        static std::mutex& mutex() {
            static std::mutex instance;
            return instance;
        }
        static std::map<std::thread::id, std::weak_ptr<Entry>>& waits() {
            static std::map<std::thread::id, std::weak_ptr<Entry>> instance;
            return instance;
        }
    };
    SinkId addOne(auto&& callback) {
        const std::lock_guard lock(mutex);
        auto entry = std::make_shared<Entry>(std::forward<decltype(callback)>(callback));
        // A slot is free once its sink was removed and no thread still runs the callback; a bounded table keeps the
        // snapshot and the search proportional to the sinks alive, not to every sink ever added.
        std::size_t index = 0;
        while (index < slots.size() && (slots[index].entry || (slots[index].retired && !slots[index].retired->quiet())))
            ++index;
        if (index == slots.size())
            slots.emplace_back();
        slots[index].entry      = std::move(entry);
        slots[index].retired = nullptr;
        const auto generation   = ++slots[index].generation;
        publish();
        return {index, generation};
    }
    // Called with the registry lock held.
    static void publish() {
        auto live = std::make_shared<Snapshot>();
        for (const auto& slot : slots)
            if (slot.entry)
                live->push_back(slot.entry);
        published = std::move(live);
    }
    inline static std::mutex                      mutex;
    inline static std::vector<Slot>               slots;
    inline static std::shared_ptr<const Snapshot> published;
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
struct TransportHandle { SinkId id; };
inline TransportHandle addTransport(function<void(const TransportRecord&)> write) {
    return {addSinks([write = std::move(write)](string_view t) { write(TransportRecord{t}); })};
}
inline void removeTransport(TransportHandle handle) { out.remove(handle.id); }
inline void reportTransportFailure(TransportHandle handle) { removeTransport(handle); }
inline void reportTransportOverflow() noexcept {}
} //namespace webfront::log
#endif
