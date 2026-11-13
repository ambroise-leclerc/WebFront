#pragma once

#include "LifeEngine.hpp"

#include <webfront/WebFront.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace webfront::demo::lifecuda {

/// Most generations one `life.cuda.advance` call computes: the call runs on the server thread, so the bound keeps
/// a CPU frame short enough for the page to stay in control.
inline constexpr std::uint32_t maxGenerationsPerCall = 1024;

/// What the page receives for a frame: the packed cells (a Uint8Array of side * side / 8 bytes), the generation,
/// the number of living cells and the time the generations took in C++, in microseconds.
using Frame = std::tuple<std::vector<std::uint8_t>, std::uint32_t, std::uint32_t, double>;

/// Registers the bridge of the demo; one field per connected browser, released when it disconnects.
///
/// - `life.cuda.info()` -> [side, CUDA available, device name]
/// - `life.cuda.seed(pattern, seed)` -> Frame of generation 0
/// - `life.cuda.advance(backend, generations)` -> Frame, `generations` later (0 returns the current one)
///
/// The page pulls: it asks for the next frame when it has drawn the previous one, so one frame at most is in flight
/// and none queues when the page is slower than the computation. Asking for the CUDA backend where it is
/// unavailable rejects the Promise instead of silently computing on the CPU.
template <typename App>
[[nodiscard]] UISubscription installLifeCuda(App& app) {
    using BrowserUI = typename App::UI;
    auto fields     = std::make_shared<std::map<WebLinkId, std::unique_ptr<Field>>>();
    auto fieldOf    = [fields](const BrowserUI& ui) -> Field& {
        auto& field = (*fields)[ui.id()];
        if (!field) {
            field = std::make_unique<Field>();
            field->seed(Pattern::soup);
        }
        return *field;
    };
    auto frameOf = [](const Field& field, double microseconds) {
        return Frame{field.packed(), field.generation(), field.population(), microseconds};
    };
    app.cppFunction("life.cuda.info", [] {
        return std::tuple{fieldSide, gpu::available(), cudaDevice()};
    });
    app.cppFunction("life.cuda.seed", [fieldOf, frameOf](BrowserUI ui, std::uint32_t pattern, std::uint32_t seed) {
        auto& field = fieldOf(ui);
        field.seed(static_cast<Pattern>(pattern), seed);
        return frameOf(field, 0.);
    });
    app.cppFunction("life.cuda.advance", [fieldOf, frameOf](BrowserUI ui, std::uint32_t backend, std::uint32_t generations) {
        if (generations > maxGenerationsPerCall)
            throw std::out_of_range("At most " + std::to_string(maxGenerationsPerCall) + " generations per call");
        auto&      field = fieldOf(ui);
        const auto start = std::chrono::steady_clock::now();
        field.advance(static_cast<Backend>(backend), generations);
        const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
        return frameOf(field, elapsed.count());
    });
    return app.subscribeUIClosed([fields](BrowserUI ui) {
        fields->erase(ui.id());
    });
}

}  // namespace webfront::demo::lifecuda
