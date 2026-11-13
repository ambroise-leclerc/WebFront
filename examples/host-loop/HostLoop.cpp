#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

// An application that already has its own loop adds a WebFront interface without giving it its thread
// (docs/HOST_INTEGRATION.md). start() opens the page in the system browser and returns: nothing waits for the console or
// for the interface. The loop below then publishes its state to every connected browser and stops when the page asks.

// LCOV_EXCL_START - interactive example; start(), broadcast and shutdown are covered by the unit tests.
int main() {
    using namespace std::chrono_literals;
    webfront::WebFront app;

    // Callbacks run on WebFront's server thread: they only leave a word for the host's loop.
    std::atomic<bool> stopRequested = false;
    app.cppFunction("stopHost", [&] {
        stopRequested = true;
    });
    app.onUIStarted([](webfront::WebFront::UI ui) {
        ui.addScript(R"js(
            const state = document.body.appendChild(document.createElement("p"));
            const stop = document.body.appendChild(document.createElement("button"));
            stop.textContent = "Stop the host application";
            stop.onclick = () => webFront.cppFunction("stopHost")().then(() => { state.textContent = "Host stopped."; }, console.error);
            globalThis.hostLoop = {onTick: tick => { state.textContent = `Host loop iteration ${tick}`; }};
        )js");
    });

    try {
        app.start();  // the server has its own thread; this one stays ours
    } catch (const std::exception& error) {
        std::cerr << "The interface could not be opened: " << error.what() << '\n';
        return 1;
    }

    // The host's own loop. A page that has not installed hostLoop.onTick yet simply misses the call.
    const auto onTick = app.broadcast("hostLoop.onTick");
    for (std::int32_t tick = 0; !stopRequested; ++tick) {
        onTick(tick);
        std::this_thread::sleep_for(100ms);
    }

    app.shutdown();  // stops and joins the server; outside any WebFront callback
    std::cout << "Host loop ended on request of the interface.\n";
}
// LCOV_EXCL_STOP
