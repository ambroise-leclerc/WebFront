#include "LifeCuda.hpp"

#include "../LifeFrontend.hpp"
#include "LifeCudaUI.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {
using namespace webfront::demo::lifecuda;

// Generations per second of one backend on the demo's field, without the bridge or the page.
double generationsPerSecond(Backend backend, std::uint32_t generationsPerCall) {
    using Clock = std::chrono::steady_clock;
    Field field;
    field.seed(Pattern::soup);
    field.advance(backend, generationsPerCall);  // first use: device buffers, caches
    const auto    start = Clock::now();
    std::uint32_t done  = 0;
    while (Clock::now() - start < std::chrono::seconds(2)) {
        field.advance(backend, generationsPerCall);
        done += generationsPerCall;
    }
    return done / std::chrono::duration<double>(Clock::now() - start).count();
}

int benchmark() {
    const auto device = cudaDevice();
    std::cout << "Field: " << fieldSide << " x " << fieldSide << " cells, " << fieldWords(fieldSide) * 8 / 1024 << " KiB per generation\n"
              << "CUDA device: " << (device.empty() ? "none" : device) << "\n\n"
              << "generations/call   CPU (1 thread)" << (device.empty() ? "" : "           CUDA    CUDA/CPU") << '\n';
    for (const std::uint32_t generations : {1u, 16u, 256u}) {
        const auto cpu = generationsPerSecond(Backend::cpu, generations);
        std::cout << std::fixed << std::setprecision(0) << std::setw(16) << generations << std::setw(13) << cpu << " /s";
        if (!device.empty()) {
            const auto cuda = generationsPerSecond(Backend::cuda, generations);
            std::cout << std::setw(12) << cuda << " /s" << std::setprecision(1) << std::setw(11) << cuda / cpu << 'x';
        }
        std::cout << '\n';
    }
    return 0;
}

// The page and its starting options, or nothing when the arguments are not pairs of a known option and a valid value.
std::optional<std::string> pageFor(std::span<char* const> arguments) {
    std::string page = "index.html";
    if (arguments.size() % 2 != 0)
        return std::nullopt;
    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const std::string_view option = arguments[i], value = arguments[i + 1];
        const bool             number = !value.empty() && value.size() <= 4 && value.find_first_not_of("0123456789") == std::string_view::npos;
        const bool             valid  = (option == "--backend" && (value == "cpu" || value == "cuda")) || (option == "--renderer" && (value == "webgl" || value == "canvas"))
                                        || ((option == "--generations" || option == "--pixelratio") && number);
        if (!valid)
            return std::nullopt;
        page += page.ends_with("html") ? '?' : '&';
        page += option.substr(2);
        page += '=';
        page += value;
    }
    return page;
}
}  // namespace

int main(int argc, char** argv) {
    const std::span<char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
    const bool                   benchmarkOnly = arguments.size() == 1 && std::string_view{arguments[0]} == "--benchmark";
    const auto                   page          = benchmarkOnly ? std::optional<std::string>{} : pageFor(arguments);
    if (!benchmarkOnly && !page) {
        std::cerr << "Usage: WebFrontLifeCuda [--backend cpu|cuda] [--generations <n>] [--renderer webgl|canvas] [--pixelratio <n>]\n"
                     "       WebFrontLifeCuda --benchmark\n"
                     "The first form opens the window. Defaults: CUDA when it is available, one generation per frame (up to 1024),\n"
                     "WebGL 2 when the page has it, the screen's device pixel ratio (1 draws fewer pixels, for software rendering).\n"
                     "--benchmark prints generations per second without the bridge or the page and exits.\n";
        return 2;
    }
    int status = 0;
    try {
        if (benchmarkOnly)
            status = benchmark();
        else {
            webfront::demo::LifeAppWithAssets<LifeCudaUI> app{
              webfront::WebFrontConfig{.window = {.title = "WebFront Life · CUDA", .width = 1280, .height = 800}}
            };
            auto subscription = installLifeCuda(app);
            app.show(*page);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    gpu::release();
    return status;
}
