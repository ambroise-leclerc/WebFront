#pragma once

// Evidence of a qualification scenario (issue #451): when WEBFRONT_QUALIFICATION_DIR is set, the scenario's final state is
// captured with WebFront's own window capture and recorded next to a JSON result. test/tools/Qualification.py gathers them.
#include <webfront/frontend/Capture.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace webfront::testing {

class QualificationEvidence {
public:
    /// Captures the window (from a worker thread: the event loop keeps running), then calls `then` from that thread.
    void capture(const std::string& engine, const std::string& scenario, std::function<void()> then) {
        const char* directory = std::getenv("WEBFRONT_QUALIFICATION_DIR");
        if (!directory || !*directory) {
            then();
            return;
        }
        worker = std::jthread([=, this] {
            const std::filesystem::path folder{directory};
            std::filesystem::create_directories(folder);
            const auto  result = frontend::capturePng(std::chrono::seconds{10});
            std::string image;
            if (result) {
                image = scenario + ".png";
                std::ofstream(folder / image, std::ios::binary)
                    .write(reinterpret_cast<const char*>(result->data()), static_cast<std::streamsize>(result->size()));
            }
            std::ofstream(folder / (scenario + ".json")) << "{\"engine\":\"" << engine << "\",\"scenario\":\"" << scenario << "\",\"image\":\"" << image
                                                         << "\",\"capture\":\"" << (result ? "ok" : "failed") << "\"}\n";
            then();
        });
    }

private:
    std::jthread worker;
};

}  // namespace webfront::testing
