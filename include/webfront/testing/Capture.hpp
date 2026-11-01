/// @brief Screenshots and image sequences of the web view, written to a directory owned by a test run.
#pragma once

#include "../frontend/Capture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace webfront::testing {

/**
 * @brief Writes PNG captures below one directory. Names are single path components, so a page can neither
 * leave the directory nor choose a path. A recording runs on its own thread and never blocks the bridge.
 */
class CaptureStore {
public:
    using Source = std::function<frontend::CaptureResult(std::chrono::milliseconds)>;

    static constexpr int         maxFramesPerSecond = 30;
    static constexpr std::size_t maxFrames          = 1800;
    /// How long the destructor waits for a capture still blocked on the window before it leaves that worker to end alone.
    static constexpr std::chrono::milliseconds shutdownGrace{500};

    explicit CaptureStore(
        std::filesystem::path root,
        Source                source =
            [](std::chrono::milliseconds timeout) {
                return frontend::capturePng(timeout);
            },
        std::size_t maxImages = maxFrames)
        : shared(std::make_shared<Shared>(std::move(root), std::move(source), maxImages)) { }
    CaptureStore(const CaptureStore&)            = delete;
    CaptureStore& operator=(const CaptureStore&) = delete;

    /// Asks the workers to stop and waits for them for at most `shutdownGrace`. A worker still blocked in its source
    /// (a window that no longer answers) is detached: it owns its state and ends by itself when the source returns,
    /// so a source must stay callable until then.
    ~CaptureStore() {
        shared->closing = true;
        std::scoped_lock lock{control};
        if (recording.job)
            recording.job->stop = true;
        settle(recording);
        settle(shot);
    }

    [[nodiscard]] static bool validName(std::string_view name) {
        // Explicit ASCII: std::isalnum follows the C locale, which GTK changes, and could accept bytes above 127.
        return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    }

    /// Writes `<directory>/<name>.png` on the calling thread; returns its size in bytes.
    std::size_t screenshot(std::string_view name) {
        return writeScreenshot(*shared, name);
    }

    /// Starts `screenshot(name)` on its own thread and returns at once, so a caller that must not wait (the bridge
    /// server thread) polls pollScreenshot(). One asynchronous screenshot at a time; a finished one that was never
    /// polled (its page went away) is discarded to free the slot.
    void beginScreenshot(std::string_view name) {
        requireName(name);
        std::scoped_lock lock{control};
        reap(shot);
        if (shot.thread.joinable())
            throw std::logic_error("A screenshot is already running");
        auto job    = std::make_shared<Job>();
        shot.job    = job;
        shot.thread = std::thread([state = shared, job, owned = std::string(name)] {
            try {
                job->count = writeScreenshot(*state, owned);
            } catch (const std::exception& error) {
                job->failure = error.what();
            }
            job->done = true;
        });
    }

    /// The size of the finished screenshot, or nothing while it is still running. Throws its error, if any.
    std::optional<std::size_t> pollScreenshot() {
        std::scoped_lock lock{control};
        if (!shot.thread.joinable())
            throw std::logic_error("No screenshot is running");
        if (!shot.job->done)
            return std::nullopt;
        shot.thread.join();
        if (!shot.job->failure.empty())
            throw std::runtime_error(shot.job->failure);
        return shot.job->count.load();
    }

    /// Starts `<directory>/<name>/0001.png`, `0002.png`, ... at the given rate until it is stopped. A name is used once
    /// per store, so a later recording can never erase the images of an earlier one; a finished recording that was
    /// never collected (its page went away) is discarded to free the slot.
    void startRecording(std::string_view name, double framesPerSecond) {
        requireName(name);
        if (!(framesPerSecond >= 1 && framesPerSecond <= maxFramesPerSecond))
            throw std::invalid_argument("The frame rate must be between 1 and 30 images per second");
        std::scoped_lock lock{control};
        reap(recording);
        if (recording.thread.joinable())
            throw std::logic_error("A recording is already running");
        // Windows and the usual macOS volumes ignore case, so "run" and "Run" are one directory there: compare folded
        // names everywhere, before anything is erased, so that the rule does not depend on the platform.
        const auto folded = foldCase(name);
        if (recordedNames.contains(folded))
            throw std::logic_error("A recording named '" + std::string(name) + "' (or differing only by case) already exists in this run");
        const auto target = shared->directory / std::string(name);
        std::filesystem::remove_all(target);  // Leftovers of an earlier run, never of this store.
        std::filesystem::create_directories(target);
        recordedNames.insert(folded);
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / framesPerSecond));
        auto       job    = std::make_shared<Job>();
        recording.job     = job;
        recording.thread  = std::thread([state = shared, job, target, period] {
            record(*state, *job, target, period);
            job->done = true;
        });
    }

    /// Asks the recording to stop without waiting; pollRecording() reports when it has. Also valid after the
    /// recording ended by itself (error or image limit).
    void requestStop() {
        std::scoped_lock lock{control};
        if (!recording.thread.joinable())
            throw std::logic_error("No recording is running");
        recording.job->stop = true;
    }

    /// Stops a running recording, if there is one, without an error, and waits for its worker for at most `shutdownGrace`
    /// so that the slot is free when this returns. Used when the page under test is replaced: its abandoned recording
    /// ends and the new page can start its own at once. A worker still blocked in its source keeps the slot until it
    /// returns (see reap()).
    void abandon() {
        std::scoped_lock lock{control};
        if (!recording.thread.joinable())
            return;
        recording.job->stop = true;
        const auto deadline = std::chrono::steady_clock::now() + shutdownGrace;
        while (!recording.job->done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        reap(recording);
    }

    /// The number of images written once the recording has ended, or nothing while it still runs. Throws the capture
    /// error that ended it, if any.
    std::optional<std::size_t> pollRecording() {
        std::scoped_lock lock{control};
        if (!recording.thread.joinable())
            throw std::logic_error("No recording is running");
        if (!recording.job->done)
            return std::nullopt;
        return finish();
    }

    /// Stops the recording, waits for it and returns the number of images written. Throws the capture error that
    /// ended it, if any.
    std::size_t stopRecording() {
        std::scoped_lock lock{control};
        if (!recording.thread.joinable())
            throw std::logic_error("No recording is running");
        recording.job->stop = true;
        return finish();
    }

private:
    struct Shared {
        Shared(std::filesystem::path root, Source source, std::size_t maxImages)
            : directory(std::move(root)), capture(std::move(source)), limit(maxImages) { }
        std::filesystem::path directory;
        Source                capture;
        std::size_t           limit;
        std::atomic<bool>     closing = false;
    };

    /// One worker's own state, shared with its thread: a new recording never sees the stop request of an old one.
    struct Job {
        std::atomic<bool>        stop  = false;
        std::atomic<bool>        done  = false;
        std::atomic<std::size_t> count = 0;  // Images written, or the screenshot's size.
        std::string              failure;    // Written before `done`.
    };

    struct Slot {
        std::thread          thread;
        std::shared_ptr<Job> job;
    };

    /// Names are ASCII (validName), so folding is exact.
    static std::string foldCase(std::string_view name) {
        std::string folded(name);
        for (auto& c : folded)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return folded;
    }

    static void requireName(std::string_view name) {
        if (!validName(name))
            throw std::invalid_argument("A capture name has 1 to 64 letters, digits, '_' or '-'");
    }

    /// Joins a worker that has already ended, freeing its slot. `control` is held.
    static void reap(Slot& slot) {
        if (slot.thread.joinable() && slot.job->done)
            slot.thread.join();
    }

    static void settle(Slot& slot) {
        if (!slot.thread.joinable())
            return;
        const auto deadline = std::chrono::steady_clock::now() + shutdownGrace;
        while (!slot.job->done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (slot.job->done)
            slot.thread.join();
        else
            slot.thread.detach();
    }

    std::size_t finish() {  // `control` is held.
        recording.thread.join();
        if (!recording.job->failure.empty())
            throw std::runtime_error("Recording stopped: " + recording.job->failure);
        return recording.job->count;
    }

    static std::size_t writeScreenshot(Shared& state, std::string_view name) {
        requireName(name);
        std::filesystem::create_directories(state.directory);
        return write(state, state.directory / (std::string(name) + ".png"));
    }

    static std::size_t write(Shared& state, const std::filesystem::path& path) {
        auto image = state.capture(std::chrono::seconds{10});
        if (!image)
            throw std::runtime_error("Capture failed: " + image.error());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write " + path.string());
        return image->size();
    }

    static void record(Shared& state, Job& job, const std::filesystem::path& target, std::chrono::steady_clock::duration period) {
        const auto ended = [&] {
            return job.stop || state.closing;
        };
        auto next = std::chrono::steady_clock::now();
        try {
            while (!ended() && job.count < state.limit) {
                auto name = std::to_string(job.count + 1);
                write(state, target / (std::string(4 - std::min<std::size_t>(name.size(), 4), '0') + name + ".png"));
                ++job.count;
                next += period;
                // A slow capture drops the missed instants instead of bursting to catch up.
                next = std::max(next, std::chrono::steady_clock::now());
                while (!ended() && std::chrono::steady_clock::now() < next)
                    std::this_thread::sleep_for(
                        std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds{5}, next - std::chrono::steady_clock::now()));
            }
        } catch (const std::exception& error) {
            job.failure = error.what();
        }
    }

    std::shared_ptr<Shared> shared;
    std::mutex              control;
    Slot                    recording;
    Slot                    shot;
    std::set<std::string>   recordedNames;
};

}  // namespace webfront::testing
