/// @brief Optional native pseudoterminal for an application-selected program.
#pragma once

#ifdef _WIN32
    #include "components/pty/Windows.hpp"
#else
    #include "components/pty/Unix.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>

namespace webfront::pty::detail {

class ByteRing {
public:
    explicit ByteRing(std::size_t capacity) : storage(capacity) {}

    std::size_t size() const {
        return used;
    }
    std::size_t capacity() const {
        return storage.size();
    }

    void append(std::span<const std::uint8_t> bytes) {
        const auto tail  = (head + used) % capacity();
        const auto first = std::min(bytes.size(), capacity() - tail);
        std::ranges::copy(bytes.first(first), storage.begin() + static_cast<std::ptrdiff_t>(tail));
        std::ranges::copy(bytes.subspan(first), storage.begin());
        used += bytes.size();
    }

    void copy(std::size_t offset, std::span<std::uint8_t> bytes) const {
        const auto start  = (head + offset) % capacity();
        const auto first  = std::min(bytes.size(), capacity() - start);
        auto       source = std::span{storage};
        std::ranges::copy(source.subspan(start, first), bytes.begin());
        std::ranges::copy(source.first(bytes.size() - first), bytes.subspan(first).begin());
    }

    void consume(std::size_t count) {
        head  = (head + count) % capacity();
        used -= count;
    }

private:
    std::vector<std::uint8_t> storage;
    std::size_t               head = 0;
    std::size_t               used = 0;
};
}  // namespace webfront::pty::detail

namespace webfront {

class Pty {
public:
    static constexpr std::size_t inputCapacity  = 64 * 1024;
    static constexpr std::size_t replayCapacity = 1024 * 1024;
    static constexpr std::size_t readChunk      = 16 * 1024;

    struct Output {
        std::uint64_t             next = 0;
        std::vector<std::uint8_t> bytes;
        bool                      truncated = false;
        std::optional<int>        exitCode;
    };

    explicit Pty(PtyOptions options)
        : native(pty::detail::prepare(std::move(options))), worker([this] {
              run();
          }) {}

    ~Pty() {
        hangUp();
    }

    Pty(const Pty&)            = delete;
    Pty& operator=(const Pty&) = delete;

    /// Enqueues the complete chunk or returns false. Never waits for the child to read.
    [[nodiscard]] bool write(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > inputCapacity)
            return false;
        std::scoped_lock lock(mutex);
        if (closing || inputClosed || bytes.size() > inputCapacity - input.size())
            return false;
        input.append(bytes);
        wakeWorker();
        return true;
    }

    /// Requests a size change on the worker. Rapid requests are coalesced.
    void resize(PtySize size) {
        pty::detail::validateSize(size);
        std::scoped_lock lock(mutex);
        if (closing || inputClosed)
            return;
        requestedSize = size;
        wakeWorker();
    }

    /// Interrupts pending reads and starts cleanup. Destruction joins the worker.
    void hangUp() {
        std::scoped_lock lock(mutex);
        if (closing || status)
            return;
        closing = true;
        wakeWorker();
        changed.notify_all();
    }

    [[nodiscard]] std::optional<int> exitCode() const {
        std::scoped_lock lock(mutex);
        return status;
    }

    /// The cursor is a byte offset, initially zero. Multiple readers can replay independently.
    [[nodiscard]] Output tryRead(std::uint64_t cursor) const {
        std::scoped_lock lock(mutex);
        return snapshot(cursor);
    }

    /// Waits for output or exit. A stop request or hangUp also wakes the wait.
    [[nodiscard]] Output read(std::uint64_t cursor, std::stop_token stop = {}) const {
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [&] {
            return cursor != end || status || closing;
        });
        return snapshot(cursor);
    }

private:
    mutable std::mutex                  mutex;
    mutable std::condition_variable_any changed;
    pty::detail::ByteRing               input{inputCapacity};
    pty::detail::ByteRing               replay{replayCapacity};
    std::uint64_t                       end         = 0;
    bool                                closing     = false;
    bool                                inputClosed = false;
    std::optional<PtySize>              requestedSize;
    std::optional<int>                  status;
    std::exception_ptr                  failure;
    pty::detail::NativePty              native;
#ifdef _WIN32
    std::condition_variable workerChanged;
#endif
    std::jthread worker;

    Output snapshot(std::uint64_t cursor) const {
        if (cursor > end)
            throw std::invalid_argument("PTY cursor is ahead of its output");
        const auto oldest = end - replay.size();
        const auto start  = std::max(cursor, oldest);
        const auto count  = static_cast<std::size_t>(std::min<std::uint64_t>(end - start, readChunk));
        Output     result{start + count, std::vector<std::uint8_t>(count), cursor < oldest, {}};
        if (count)
            replay.copy(static_cast<std::size_t>(start - oldest), result.bytes);
        if (!count && failure)
            std::rethrow_exception(failure);
        if (result.next == end && !failure)
            result.exitCode = status;
        return result;
    }

    void publish(std::span<const std::uint8_t> bytes) {
        if (bytes.empty())
            return;
        std::scoped_lock lock(mutex);
        const auto       required = replay.size() + bytes.size();
        if (required > replay.capacity())
            replay.consume(required - replay.capacity());
        replay.append(bytes);
        end += bytes.size();
        changed.notify_all();
    }

    void wakeWorker() {
#ifdef _WIN32
        workerChanged.notify_one();
#else
        native.wake();
#endif
    }

    void run() {
        using namespace std::chrono_literals;
        std::exception_ptr                  error;
        std::array<std::uint8_t, readChunk> output{};
        std::array<std::uint8_t, 4096>      incoming{};
        try {
            std::optional<std::chrono::steady_clock::time_point> drainDeadline;
            for (;;) {
                if (!drainDeadline && native.exited()) {
                    {
                        std::scoped_lock lock(mutex);
                        inputClosed = true;
                    }
                    native.beginDrain();
                    drainDeadline = std::chrono::steady_clock::now() + 3s;
                }
                const auto received = native.read(output);
                publish(std::span{output}.first(received));
                if (drainDeadline && (native.atEnd() || std::chrono::steady_clock::now() >= *drainDeadline))
                    break;

                std::optional<PtySize> size;
                std::size_t            count = 0;
                {
                    std::scoped_lock lock(mutex);
                    if (closing)
                        break;
                    if (!drainDeadline) {
                        size  = std::exchange(requestedSize, std::nullopt);
                        count = std::min(input.size(), incoming.size());
                        if (count)
                            input.copy(0, std::span{incoming}.first(count));
                    }
                }
                if (size)
                    native.resize(*size);
                const auto sent = count ? native.write(std::span{incoming}.first(count)) : 0;
                {
                    std::scoped_lock lock(mutex);
                    input.consume(sent);
                }
                if (!received && !sent) {
#ifdef _WIN32
                    std::unique_lock lock(mutex);
                    if (!closing && (drainDeadline || (!requestedSize && (count != 0 || input.size() == 0))))
                        workerChanged.wait_for(lock, drainDeadline || count ? 10ms : 40ms);
#else
                    native.wait(count != 0, drainDeadline.has_value());
#endif
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::scoped_lock lock(mutex);
            inputClosed = true;
        }
        const int        code = native.finish();
        std::scoped_lock lock(mutex);
#ifndef _WIN32
        native.closeWakePipe();
#endif
        status  = code;
        failure = error;
        changed.notify_all();
    }
};
}  // namespace webfront
