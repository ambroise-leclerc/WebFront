// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-13
//
// WebFront — examples
// Role: Host-loop example application: runs the Boing Ball beside a native window or a system browser, or as a benchmark.
// Thread: ui-loop | Alloc: unrestricted
// LLM: example entry point; see docs/HOST_INTEGRATION.md for the three integration paths

#include "../PortOption.hpp"
#include "HostLoopCapture.hpp"
#include "HostLoopUI.hpp"
#include <webfront/frontend/Capture.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

// An application that already has its own loop adds a WebFront interface without giving it its thread
// (docs/HOST_INTEGRATION.md). The host here ray-traces a Boing Ball, on the CPU or with CUDA (BoingHost.hpp): its loop
// advances the scene whether or not a browser watches, and computes an image when one asks for it. WebFront only
// carries the words of the page to the loop and the pixels of the loop to the page (HostLoopUI.hpp).
//
// The interface is opened in one of two ways, and the loop is the same in both:
// - the system browser: start() returns at once and the loop keeps the main thread;
// - the platform's native window, when its engine is built: the toolkit owns the main thread, so the loop runs on a
//   thread of the host, which closes the window when the loop ends.
// This is a development build of WebFront: see docs/WEBKITGTK.md before distributing a native window.

// LCOV_EXCL_START - interactive example; the host, its bridge and the page are covered by HostLoopTests.cpp and the browser specs.
namespace {
using namespace std::chrono_literals;

struct Options {
    boing::Settings settings;
    bool            browser{false}, stats{false}, benchmark{false};
    unsigned        captureFrames{1};
    bool            captureDemo{false};
    std::string     capture;  // a PNG file to write from the native window, after which the host stops
};

// The application has no page of its own: it empties WebFront's embedded one and adds the host's interface to it.
const std::string ownPage = "document.body.replaceChildren();" + std::string{boing::page};

void printStats(const boing::Frame& frame, const boing::Displayed& displayed) {
    std::printf("%s %dx%d rays/pixel %d %s | compute %.1f ms, copy %.1f ms, pack %.1f ms, bridge %.1f ms, draw %.1f ms | displayed %.0f images/s\n",
                frame.settings.engine == boing::Engine::cuda ? "CUDA" : "CPU",
                frame.scene.width,
                frame.scene.height,
                frame.scene.samples * frame.scene.samples,
                frame.settings.glass ? "glass, pixels" : "opaque, runs",
                frame.computeMs,
                frame.copyMs,
                frame.packMs,
                displayed.bridgeMs,
                displayed.drawMs,
                displayed.framesPerSecond);
    std::fflush(stdout);
}

// The system browser: nothing waits for the console or for the interface, and this thread stays the host's.
int runWithBrowser(boing::Host& host, const Options& options) {
    webfront::WebFront app;
    boing::install(app, host);
    app.onUIStarted([](webfront::WebFront::UI ui) {
        ui.addScript(ownPage);
    });
    app.start();
    host.run({}, options.stats ? printStats : nullptr);  // the host's own loop, until the page asks it to stop
    app.shutdown();                                      // stops and joins the server; outside any WebFront callback
    std::cout << "Host loop ended on request of the interface.\n";
    return 0;
}

// The native window: show() keeps the main thread until the window closes, whoever closes it.
int runWithWindow(boing::Host& host, const Options& options) {
    using App = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
    // The server's connection handler refers to these; keep them alive until after app is destroyed.
    std::mutex             uiMutex;
    std::optional<App::UI> captureUI;
    App                    app{webfront::WebFrontConfig{
        .window = {.title = "WebFront host loop", .width = options.captureDemo ? 640 : 800, .height = options.captureDemo ? 480 : 600}}};
    boing::install(app, host);
    app.onUIStarted([&](App::UI ui) {
        const std::scoped_lock lock{uiMutex};
        captureUI              = ui;
        const std::string mode = options.captureFrames > 1
                                     ? (options.settings.glass ? "globalThis.hostLoopCapture = {glass: true};" : "globalThis.hostLoopCapture = {glass: false};")
                                     : "";
        const std::string style = options.captureDemo ? "document.querySelector('#host-loop .stats').remove();" : "";
        ui.addScript(mode + ownPage + style);
    });
    std::atomic<bool>  showReturned{false};
    std::exception_ptr loopFailure;
    std::jthread       loop([&](std::stop_token stopping) {
        try {
            host.run(stopping, options.stats ? printStats : nullptr, options.captureFrames > 1 ? 1. / 25. : 0.);
        } catch (...) {
            loopFailure = std::current_exception();
        }
        // The loop ended first: close the window. It may not exist yet, and such a request is not kept, so repeat it.
        while (!showReturned && !stopping.stop_requested()) {
            app.requestClose();
            std::this_thread::sleep_for(10ms);
        }
    });
    // --capture: WebFront itself gives a PNG of what the window shows, to any thread but the window's. The host takes
    // one or a sequence after six seconds of animation, then ends its loop, which closes the window.
    std::exception_ptr captureFailure;
    bool               captureCompleted = false;  // read only after the capturer is joined
    std::jthread       capturer;
    if (!options.capture.empty())
        capturer = std::jthread([&](std::stop_token stopping) {
            std::mutex                  mutex;
            std::condition_variable_any closed;
            std::unique_lock            lock{mutex};
            closed.wait_for(lock, stopping, 6s, [] {
                return false;
            });
            if (stopping.stop_requested())
                return;
            try {
                std::optional<App::UI> ui;
                {
                    const std::scoped_lock uiLock{uiMutex};
                    ui = captureUI;
                }
                if (!ui)
                    throw std::runtime_error("No capture: the browser did not connect");
                for (unsigned i = 0; i < options.captureFrames; ++i) {
                    if (stopping.stop_requested())
                        return;
                    if (options.captureFrames > 1) {
                        const bool glass = options.captureDemo ? i >= options.captureFrames * 2 / 3 : options.settings.glass;
                        auto       drawn = ui->jsFunction<std::uint32_t>("hostLoop.captureNext")(glass);
                        if (drawn.wait_for(10s) != std::future_status::ready)
                            throw std::runtime_error("No capture: the browser did not draw the next frame");
                        (void)drawn.get();
                    }
                    boing::saveCapture(webfront::frontend::capturePng(), boing::captureFramePath(options.capture, i, options.captureFrames));
                }
                captureCompleted = true;
                std::cout << "Captured the window in " << options.capture << '\n';
            } catch (...) {
                captureFailure = std::current_exception();
            }
            host.stop();
        });
    app.show();
    showReturned = true;
    capturer     = {};    // stopped and joined
    loop.request_stop();  // the user closed the window first
    loop.join();          // no call into WebFront is running when the application is destroyed
    if (loopFailure)
        std::rethrow_exception(loopFailure);
    if (captureFailure)
        std::rethrow_exception(captureFailure);
    if (!options.capture.empty() && !captureCompleted)
        throw std::runtime_error("No capture: the window closed before capture completed");
    return 0;
}

// Images per second of each engine without any interface: the computation alone, then with the copy to host memory.
int benchmark(boing::Host& host) {
    std::cout << "CPU threads: " << host.threads() << "\nCUDA device: " << (host.device().empty() ? "none" : host.device()) << "\n\n"
              << "image      rays  ball     engine  compute ms  copy ms\n";
    for (const auto quality : boing::qualities)
        for (const bool glass : {false, true})
            for (const auto engine : {boing::Engine::cpu, boing::Engine::cuda}) {
                if (engine == boing::Engine::cuda && host.device().empty())
                    continue;
                boing::Frame frame;
                frame.settings.engine = engine;
                double    compute = 0, copy = 0;
                const int images = 60;
                for (int i = -3; i < images; ++i) {  // three unmeasured images: device memory, caches, frequency
                    frame.scene = boing::sceneAt(static_cast<float>(i) / 30.F, glass, quality.width, quality.height, quality.samples);
                    boing::render(frame, host.threads());
                    compute += i < 0 ? 0 : frame.computeMs;
                    copy    += i < 0 ? 0 : frame.copyMs;
                }
                std::printf("%4dx%-4d  %4d  %-7s  %-6s  %10.2f  %7.2f\n",
                            quality.width,
                            quality.height,
                            quality.samples * quality.samples,
                            glass ? "glass" : "opaque",
                            engine == boing::Engine::cuda ? "CUDA" : "CPU",
                            compute / images,
                            copy / images);
            }
    return 0;
}

bool parse(std::span<char* const> arguments, Options& options) {
    bool captureFramesSpecified = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        const std::string_view value    = i + 1 < arguments.size() ? arguments[i + 1] : "";
        if (argument == "--engine" && (value == "cpu" || value == "cuda"))
            options.settings.engine = value == "cuda" ? boing::Engine::cuda : boing::Engine::cpu, ++i;
        else if (argument == "--quality" && value.size() == 1 && value[0] >= '0' && value[0] < '0' + static_cast<char>(boing::qualities.size()))
            options.settings.quality = static_cast<std::uint32_t>(value[0] - '0'), ++i;
        else if (argument == "--glass")
            options.settings.glass = true;
        else if (argument == "--capture" && !value.empty())
            options.capture = value, ++i;
        else if (argument == "--capture-demo")
            options.captureDemo = true;
        else if (argument == "--capture-frames" && !value.empty()) {
            const auto count = boing::captureFrameCount(value);
            if (!count)
                return false;
            options.captureFrames  = *count;
            captureFramesSpecified = true;
            ++i;
        } else if (argument == "--browser")
            options.browser = true;
        else if (argument == "--stats")
            options.stats = true;
        else if (argument == "--benchmark")
            options.benchmark = true;
        else
            return false;
    }
    if (options.captureDemo && !captureFramesSpecified)
        options.captureFrames = 300;
    return (!(captureFramesSpecified || options.captureDemo) || !options.capture.empty())
           && (!options.captureDemo || (options.captureFrames >= 3 && options.captureFrames % 3 == 0));
}
}  // namespace

int main(int argc, char** argv) {
    if (!webfront::demo::applyPortOption(argc, argv))
        return 2;
    Options options;
    if (!parse({argv + 1, static_cast<std::size_t>(argc - 1)}, options)) {
        std::cerr << "Usage: WebFrontHostLoop [--port <n>] [--engine cpu|cuda] [--quality 0-3] [--glass] [--browser] [--stats] [--capture <file.png>] "
                     "[--capture-frames 1-600] [--capture-demo]\n"
                     "       WebFrontHostLoop --benchmark\n"
                     "  --engine     the renderer at start (default cpu); cuda is an error where it is unavailable\n"
                     "  --quality    0: 320x240, 1: 640x480, 2: 640x480 with 4 rays per pixel, 3: 960x720 with 4 (default)\n"
                     "  --port       the port of the browser interface, 0 for a free port (default WEBFRONT_PORT, else 9002)\n"
                     "  --browser    open the system browser even when the native window is built\n"
                     "  --stats      print the timings of an image every second\n"
                     "  --capture    write a PNG of the native window after six seconds, then stop\n"
                     "  --capture-frames  save numbered PNGs with a fixed 25 fps scene clock (default 1); requires --capture\n"
                     "  --capture-demo  hide statistics and switch to glass after two thirds (default 300 frames)\n"
                     "  --benchmark  time both renderers without any interface, and exit\n";
        return 2;
    }
    // WebFront's warnings on the console: a development build says there that its native window has no sandbox.
    webfront::log::setLogLevel(webfront::log::Warn);
    webfront::log::addSinks(webfront::log::clogSink);
    try {
        boing::validateCapture(!options.capture.empty(), webfront::frontend::nativeWebViewAvailable, options.browser, options.benchmark);
        // Declared before the application, hence destroyed after it: the callbacks refer to the host.
        boing::Host host{options.settings};
        if (options.benchmark)
            return benchmark(host);
        if (webfront::frontend::nativeWebViewAvailable && !options.browser)
            return runWithWindow(host, options);
        return runWithBrowser(host, options);
    } catch (const std::exception& error) {
        std::cerr << "WebFrontHostLoop: " << error.what() << '\n';
        return 1;
    }
}
// LCOV_EXCL_STOP
