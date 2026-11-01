/// @brief Screenshots and image sequences of the web view, written to a directory owned by a test run.
#pragma once

#include "../frontend/Capture.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
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

    explicit CaptureStore(
        std::filesystem::path root,
        Source                source =
            [](std::chrono::milliseconds timeout) {
                return frontend::capturePng(timeout);
            },
        std::size_t maxImages = maxFrames)
        : directory(std::move(root)), capture(std::move(source)), limit(maxImages) { }
    CaptureStore(const CaptureStore&)            = delete;
    CaptureStore& operator=(const CaptureStore&) = delete;
    ~CaptureStore() {
        stopping = true;
        std::scoped_lock lock{control};
        if (worker.joinable())
            worker.join();
        if (shotWorker.joinable())
            shotWorker.join();
    }

    [[nodiscard]] static bool validName(std::string_view name) {
        return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        });
    }

    /// Writes `<directory>/<name>.png` on the calling thread; returns its size in bytes.
    std::size_t screenshot(std::string_view name) {
        requireName(name);
        std::filesystem::create_directories(directory);
        return write(directory / (std::string(name) + ".png"));
    }

    /// Starts `screenshot(name)` on its own thread and returns at once, so a caller that must not wait (the bridge
    /// server thread) polls pollScreenshot(). One asynchronous screenshot at a time.
    void beginScreenshot(std::string_view name) {
        requireName(name);
        std::scoped_lock lock{control};
        if (shotWorker.joinable())
            throw std::logic_error("A screenshot is already running");
        shotDone = false;
        shotSize = 0;
        shotFailure.clear();
        shotWorker = std::thread([this, owned = std::string(name)] {
            try {
                shotSize = screenshot(owned);
            } catch (const std::exception& error) {
                shotFailure = error.what();
            }
            shotDone = true;
        });
    }

    /// The size of the finished screenshot, or nothing while it is still running. Throws its error, if any.
    std::optional<std::size_t> pollScreenshot() {
        std::scoped_lock lock{control};
        if (!shotWorker.joinable())
            throw std::logic_error("No screenshot is running");
        if (!shotDone)
            return std::nullopt;
        shotWorker.join();
        if (!shotFailure.empty())
            throw std::runtime_error(shotFailure);
        return shotSize;
    }

    /// Starts `<directory>/<name>/0001.png`, `0002.png`, ... at the given rate until it is stopped.
    void startRecording(std::string_view name, double framesPerSecond) {
        requireName(name);
        if (!(framesPerSecond >= 1 && framesPerSecond <= maxFramesPerSecond))
            throw std::invalid_argument("The frame rate must be between 1 and 30 images per second");
        std::scoped_lock lock{control};
        if (worker.joinable())
            throw std::logic_error("A recording is already running");
        const auto target = directory / std::string(name);
        std::filesystem::remove_all(target);
        std::filesystem::create_directories(target);
        frames = 0;
        failure.clear();
        stopping          = false;
        recording         = true;
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / framesPerSecond));
        worker            = std::thread([this, target, period] {
            record(target, period);
            recording = false;
        });
    }

    /// Asks the recording to stop without waiting; pollRecording() reports when it has. Also valid after the
    /// recording ended by itself (error or image limit).
    void requestStop() {
        std::scoped_lock lock{control};
        if (!worker.joinable())
            throw std::logic_error("No recording is running");
        stopping = true;
    }

    /// The number of images written once the recording has ended, or nothing while it still runs. Throws the capture
    /// error that ended it, if any.
    std::optional<std::size_t> pollRecording() {
        std::scoped_lock lock{control};
        if (!worker.joinable())
            throw std::logic_error("No recording is running");
        if (recording)
            return std::nullopt;
        return finish();
    }

    /// Stops the recording, waits for it and returns the number of images written. Throws the capture error that
    /// ended it, if any.
    std::size_t stopRecording() {
        std::scoped_lock lock{control};
        if (!worker.joinable())
            throw std::logic_error("No recording is running");
        stopping = true;
        return finish();
    }

private:
    static void requireName(std::string_view name) {
        if (!validName(name))
            throw std::invalid_argument("A capture name has 1 to 64 letters, digits, '_' or '-'");
    }

    std::size_t finish() {  // `control` is held.
        worker.join();
        if (!failure.empty())
            throw std::runtime_error("Recording stopped: " + failure);
        return frames;
    }

    std::size_t write(const std::filesystem::path& path) {
        auto image = capture(std::chrono::seconds{10});
        if (!image)
            throw std::runtime_error("Capture failed: " + image.error());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write " + path.string());
        return image->size();
    }

    void record(const std::filesystem::path& target, std::chrono::steady_clock::duration period) {
        auto next = std::chrono::steady_clock::now();
        try {
            while (!stopping && frames < limit) {
                auto name = std::to_string(frames + 1);
                write(target / (std::string(4 - std::min<std::size_t>(name.size(), 4), '0') + name + ".png"));
                ++frames;
                next += period;
                // A slow capture drops the missed instants instead of bursting to catch up.
                next = std::max(next, std::chrono::steady_clock::now());
                while (!stopping && std::chrono::steady_clock::now() < next)
                    std::this_thread::sleep_for(
                        std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds{5}, next - std::chrono::steady_clock::now()));
            }
        } catch (const std::exception& error) {
            failure = error.what();
        }
    }

    std::filesystem::path    directory;
    Source                   capture;
    std::size_t              limit;
    std::mutex               control;
    std::thread              worker;
    std::atomic<bool>        stopping  = false;
    std::atomic<bool>        recording = false;
    std::atomic<std::size_t> frames    = 0;
    std::string              failure;
    std::thread              shotWorker;
    std::atomic<bool>        shotDone = false;
    std::size_t              shotSize = 0;
    std::string              shotFailure;
};

}  // namespace webfront::testing
