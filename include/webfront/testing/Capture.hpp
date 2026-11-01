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

    explicit CaptureStore(std::filesystem::path root, Source source = [](std::chrono::milliseconds timeout) { return frontend::capturePng(timeout); })
        : directory(std::move(root)), capture(std::move(source)) {}
    CaptureStore(const CaptureStore&)            = delete;
    CaptureStore& operator=(const CaptureStore&) = delete;
    ~CaptureStore() {
        try {
            stopRecording();
        } catch (...) {
        }
    }

    [[nodiscard]] static bool validName(std::string_view name) {
        return !name.empty() && name.size() <= 64 &&
               std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

    /// Writes `<directory>/<name>.png`; returns its size in bytes.
    std::size_t screenshot(std::string_view name) {
        requireName(name);
        std::filesystem::create_directories(directory);
        return write(directory / (std::string(name) + ".png"));
    }

    /// Starts `<directory>/<name>/0001.png`, `0002.png`, ... at the given rate until stopRecording().
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
        stopping = false;
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / framesPerSecond));
        worker            = std::thread([this, target, period] { record(target, period); });
    }

    /// Stops the recording and returns the number of images written. Throws the capture error that ended it, if any.
    std::size_t stopRecording() {
        std::scoped_lock lock{control};
        if (!worker.joinable())
            throw std::logic_error("No recording is running");
        stopping = true;
        worker.join();
        if (!failure.empty())
            throw std::runtime_error("Recording stopped: " + failure);
        return frames;
    }

private:
    static void requireName(std::string_view name) {
        if (!validName(name))
            throw std::invalid_argument("A capture name has 1 to 64 letters, digits, '_' or '-'");
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
            while (!stopping && frames < maxFrames) {
                auto name = std::to_string(frames + 1);
                write(target / (std::string(4 - std::min<std::size_t>(name.size(), 4), '0') + name + ".png"));
                ++frames;
                next += period;
                // A slow capture drops the missed instants instead of bursting to catch up.
                next = std::max(next, std::chrono::steady_clock::now());
                while (!stopping && std::chrono::steady_clock::now() < next)
                    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds{5}, next - std::chrono::steady_clock::now()));
            }
        } catch (const std::exception& error) {
            failure = error.what();
        }
    }

    std::filesystem::path directory;
    Source                capture;
    std::mutex            control;
    std::thread           worker;
    std::atomic<bool>     stopping = false;
    std::atomic<std::size_t> frames = 0;
    std::string           failure;
};

}  // namespace webfront::testing
