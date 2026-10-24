// ADR-017 (#363): what the transport benchmarks of every engine share. The page (transport-benchmark.html) runs the same
// echo calls through the HTML bridge (WebSocket to the embedded server) and through the engine's own message channel,
// which the engine's benchmark registers before navigation under the names wfecho and wfrun. This header holds the
// application side: the bridge callbacks, the C++ to JS bridge loop and the report. It fails on a wrong echo, never on a
// timing.
#pragma once

#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace transport_benchmark {

using Clock = std::chrono::steady_clock;

template <typename Frontend>
using App = webfront::BasicWFWithFrontend<webfront::NetProvider, webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>, Frontend>;

// Each call awaits the previous result, as the direct run does; the page receives the elapsed nanoseconds, or -1.
template <typename Application>
webfront::Task cppToJs(typename Application::UI ui, int size, int count) {
    const std::string payload(static_cast<std::size_t>(size), 'x');
    double            elapsed = -1;
    try {
        const auto start = Clock::now();
        for (int i = 0; i < count; ++i)
            if (co_await ui.template jsFunctionSender<std::string>("benchmark.echo")(payload) != payload)
                throw std::runtime_error("wrong echo");
        elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    } catch (const std::exception& error) {
        std::cerr << "C++ to JS bridge echo failed: " << error.what() << '\n';
    }
    ui.jsFunction("benchmark.cppPhaseDone")(elapsed);
}

struct Report {
    bool        received = false;
    bool        passed   = false;
    std::string json;
    std::string text;
};

/// Serves the page from the working directory (test/web) in the frontend's window; the frontend registers its direct channel.
template <typename Frontend>
int run(int argc, char** argv, const char* usage) {
    try {
        if (argc > 2)
            throw std::runtime_error(usage);
        Report         report;
        App<Frontend> app("9002", std::filesystem::current_path());
        app.template cppFunction<std::string, std::string>("echo", [](const std::string& value) {
            return value;
        });
        app.cppFunction("cppToJs", [&app](typename App<Frontend>::UI ui, int size, int count) {
            app.spawn(cppToJs<App<Frontend>>(ui, size, count));
        });
        app.template cppFunction<void, bool, std::string, std::string>("report", [&report](bool passed, const std::string& json, const std::string& text) {
            report = {true, passed, json, text};
        });
        // show() returns once the window is closed and the server joined: the report is no longer written to.
        app.show("transport-benchmark.html");
        if (!report.received)
            throw std::runtime_error("the benchmark page closed without a report");
        std::cout << report.text << '\n';
        if (!report.passed)
            return 1;
        if (argc == 2) {
            std::ofstream output(argv[1], std::ios::binary);
            output << report.json << '\n';
            if (!output)
                throw std::runtime_error(std::string("cannot write ") + argv[1]);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

}  // namespace transport_benchmark
