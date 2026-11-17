#pragma once

// The host application of the example: it owns the scene, its clock, the renderers and its loop. Nothing here knows
// WebFront. Other threads only leave words for the loop (settings, a request for the next image, a stop); the loop
// reads them at the start of an iteration, so no image is ever computed on the thread that asked for it.

#include "BoingCuda.hpp"
#include "BoingScene.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace boing {

enum class Engine : std::uint32_t { cpu, cuda };

/// The image sizes and rays per pixel (samples x samples) a browser may choose.
struct Quality {
    int width, height, samples;
};
inline constexpr std::array<Quality, 4> qualities{
    {{320, 240, 1}, {640, 480, 1}, {640, 480, 2}, {960, 720, 2}}
};

struct Settings {
    Engine        engine{Engine::cpu};
    bool          paused{false};
    bool          glass{false};
    std::uint32_t quality{3};  ///< an index in `qualities`; the largest by default
};

/// An image and how it was obtained. `computeMs` is the whole computation (for CUDA, until the device has finished);
/// `copyMs` is what bringing the pixels back to host memory costs (nothing on the CPU, which computes them in place);
/// `packMs` is the run-length encoding.
struct Frame {
    std::uint32_t              sequence{};
    std::uint32_t              revision{};  ///< of the settings: how many times they had been set
    Settings                   settings;
    Scene                      scene{};
    double                     computeMs{}, copyMs{}, packMs{};
    std::vector<std::uint32_t> pixels;  ///< width * height, the bytes R, G, B, A of each pixel
    std::vector<std::uint32_t> packed;  ///< the same pixels as runs, or empty when the image is sent as it is
};

/// Run-length encoding: each run of equal pixels becomes its length and the pixel. The opaque ball stands before flat
/// colours, whose runs are long; the beams of the glass ball leave no two neighbours alike, and its image is sent as
/// it is, since its runs would take more room than its pixels.
inline void pack(const std::vector<std::uint32_t>& pixels, std::vector<std::uint32_t>& runs) {
    runs.clear();
    for (std::size_t start = 0, end = 0; start < pixels.size(); start = end) {
        for (end = start + 1; end < pixels.size() && pixels[end] == pixels[start];)
            ++end;
        runs.push_back(static_cast<std::uint32_t>(end - start));
        runs.push_back(pixels[start]);
    }
}

/// What a browser measured for the image it showed last: the rate it really displays, the time to draw an image, and
/// the rest of the time between its request and the image (the bridge both ways and the wait for the loop).
struct Displayed {
    double framesPerSecond{}, drawMs{}, bridgeMs{};
};

/// Every pixel on the CPU: `threads` threads take the rows one by one, since rows that cross the ball cost more.
inline void renderCpu(const Scene& scene, std::uint32_t* pixels, unsigned threads) {
    std::atomic<int> nextRow{0};
    const auto       rows = [&] {
        for (int y = nextRow++; y < scene.height; y = nextRow++)
            for (int x = 0; x < scene.width; ++x)
                pixels[y * scene.width + x] = pixel(scene, x, y);
    };
    std::vector<std::jthread> workers;
    for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back(rows);
    rows();
}  // the workers are joined here

/// Computes `frame.scene` into `frame.pixels` with the engine of `frame.settings`, and times it.
inline void render(Frame& frame, unsigned threads) {
    using Clock      = std::chrono::steady_clock;
    const auto since = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    frame.pixels.resize(static_cast<std::size_t>(frame.scene.width) * static_cast<std::size_t>(frame.scene.height));
    const auto start = Clock::now();
    if (frame.settings.engine == Engine::cpu) {
        renderCpu(frame.scene, frame.pixels.data(), threads);
        frame.computeMs = since(start);
        frame.copyMs    = 0;
        return;
    }
    if (const char* error = cudaCompute(frame.scene))
        throw std::runtime_error(std::string{"CUDA computation failed: "} + error);
    frame.computeMs = since(start);
    const auto copy = Clock::now();
    if (const char* error = cudaCopy(frame.pixels.data(), frame.pixels.size()))
        throw std::runtime_error(std::string{"CUDA copy failed: "} + error);
    frame.copyMs = since(copy);
}

class Host {
public:
    using Deliver = std::function<void(const Frame&)>;

    /// Throws like set() when the settings cannot be honoured.
    explicit Host(Settings initial = {}, unsigned cpuThreads = std::max(1U, std::thread::hardware_concurrency()))
        : cudaName(cudaDevice()), workers(cpuThreads) {
        set(initial);
    }
    ~Host() {
        cudaRelease();
    }
    Host(const Host&)            = delete;
    Host& operator=(const Host&) = delete;

    /// The CUDA device, or an empty string: without one, choosing the CUDA engine is refused.
    [[nodiscard]] const std::string& device() const {
        return cudaName;
    }
    [[nodiscard]] unsigned threads() const {
        return workers;
    }

    // --- Words left for the loop, from any thread. None of them computes anything.

    /// Replaces the settings and returns their revision: the images computed with them carry at least that revision.
    /// An unknown quality or engine, or CUDA where there is none, throws and changes nothing.
    std::uint32_t set(Settings wanted) {
        if (wanted.quality >= qualities.size())
            throw std::out_of_range("Unknown quality " + std::to_string(wanted.quality));
        if (wanted.engine != Engine::cpu && wanted.engine != Engine::cuda)
            throw std::out_of_range("Unknown engine");
        if (wanted.engine == Engine::cuda && cudaName.empty())
            throw std::runtime_error("CUDA is unavailable: this build has no CUDA renderer, or no device was found");
        const std::scoped_lock lock{mutex};
        settings = wanted;
        return ++revision;
    }

    /// `consumer` has shown its last image and wants the next one: it will receive exactly one, the first the loop
    /// computes from now on. A duplicate before delivery starts still receives one image, including during rendering.
    /// Delivery may request the next image immediately; a slow consumer makes the loop compute less, never queue.
    void request(std::uint64_t consumer, Deliver deliver, Displayed displayed = {}) {
        const std::scoped_lock lock{mutex};
        if (ended || inFlight.contains(consumer))
            return;
        waiting.insert_or_assign(consumer, std::move(deliver));
        lastDisplayed = displayed;
        wake.notify_one();
    }

    /// Asks the loop to end.
    void stop() {
        const std::scoped_lock lock{mutex};
        stopping = true;
        wake.notify_one();
    }

    // --- The loop, on the thread the application gives it.

    /// One iteration, `elapsed` seconds after the previous one: the scene's clock advances unless paused, whether or
    /// not anyone is watching, and an image is computed only if a consumer waits for it. Returns whether one was.
    /// With `frameClock`, elapsed is a fixed step applied only when an image is rendered (offline capture).
    bool iterate(double elapsed, bool frameClock = false) {
        std::map<std::uint64_t, Deliver> consumers;
        struct ReleaseInFlight {
            Host& host;
            ~ReleaseInFlight() {
                const std::scoped_lock lock{host.mutex};
                host.inFlight.clear();
            }
        } release{*this};
        {
            const std::scoped_lock lock{mutex};
            frame.settings = settings;
            frame.revision = revision;
            consumers.swap(waiting);
            for (const auto& [consumer, deliver] : consumers)
                inFlight.insert(consumer);
        }
        if (!frame.settings.paused && (!frameClock || !consumers.empty()))
            seconds += elapsed;
        if (consumers.empty())
            return false;
        const auto& quality = qualities[frame.settings.quality];
        frame.scene         = sceneAt(static_cast<float>(seconds), frame.settings.glass, quality.width, quality.height, quality.samples);
        ++frame.sequence;
        render(frame, workers);
        const auto start = std::chrono::steady_clock::now();
        frame.packed.clear();
        if (!frame.settings.glass)
            pack(frame.pixels, frame.packed);
        frame.packMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        for (const auto& [consumer, deliver] : consumers) {
            {
                const std::scoped_lock lock{mutex};
                inFlight.erase(consumer);  // delivery may immediately request the consumer's next image
            }
            deliver(frame);
        }
        return true;
    }

    /// Iterates until stop() or `stopping`, sleeping at most 10 ms while no consumer waits. `report`, if any, is called
    /// about once a second with the last image and what its consumer displayed. When it returns, pending and later
    /// requests are dropped: what they hold (a browser) may then be destroyed.
    /// A positive `frameStep` uses the offline frame clock instead of wall time.
    void run(std::stop_token external = {}, const std::function<void(const Frame&, const Displayed&)>& report = {}, double frameStep = 0.) {
        using Clock = std::chrono::steady_clock;
        struct Ending {
            Host& host;
            ~Ending() {
                const std::scoped_lock lock{host.mutex};
                host.ended = true;
                host.waiting.clear();
            }
        } ending{*this};
        auto      last = Clock::now(), reported = last;
        Displayed displayed;
        for (;;) {
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, std::chrono::milliseconds{10}, [this] {
                    return stopping || !waiting.empty();
                });
                if (stopping || external.stop_requested())
                    return;
                displayed = lastDisplayed;
            }
            const auto now      = Clock::now();
            const bool computed = iterate(frameStep > 0. ? frameStep : std::chrono::duration<double>(now - last).count(), frameStep > 0.);
            last                = now;
            if (computed && report && now - reported >= std::chrono::seconds{1}) {
                report(frame, displayed);
                reported = now;
            }
        }
    }

    /// The time of the scene, in seconds; for the loop's thread.
    [[nodiscard]] double sceneSeconds() const {
        return seconds;
    }

private:
    const std::string cudaName;
    const unsigned    workers;

    std::mutex                       mutex;  // guards the words below
    std::condition_variable          wake;
    Settings                         settings;
    std::uint32_t                    revision{0};
    std::map<std::uint64_t, Deliver> waiting;
    std::set<std::uint64_t>         inFlight;  // retained until this consumer's delivery begins
    Displayed                        lastDisplayed;
    bool                             stopping{false}, ended{false};

    double seconds{0};  // the loop's own state
    Frame  frame;
};

}  // namespace boing
