#pragma once

// The interface WebFront adds to the host: four callbacks and one page. The callbacks run on WebFront's server thread
// and only leave a word for the host's loop; the loop sends each image itself, from its own thread.
//
//   host.info()                                    -> [CUDA device or "", CPU threads]
//   host.settings(engine, paused, glass, quality)  -> revision; engine 0 CPU, 1 CUDA; rejects what the host cannot honour
//   host.nextFrame(framesPerSecond, drawMs, bridgeMs)  the calling browser showed its image and wants the next one
//   host.stop()                                    ends the host's loop
//   hostLoop.onFrame(data, [sequence, width, height, raysPerPixel, revision, packed], [engine, paused, glass, quality],
//                    [computeMs, copyMs, packMs])
//       in the page. data is a Uint32Array: the width * height pixels, whose bytes are R, G, B, A, or, when packed, their
//       runs (a length, then the pixel).

#include "BoingHost.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>

namespace boing {

/// The whole page, added to WebFront's embedded one: no local file or remote resource to load. WebFront serves it over
/// loopback HTTP and carries calls over WebSocket. The image
/// fills the window and the controls float over it. The page draws the pixels it receives, after expanding their runs
/// when they come packed, and asks for the next image only once the previous one is on screen.
inline constexpr std::string_view page = R"js(
(() => {
    const call = (name, ...args) => webFront.cppFunction(name)(...args);
    const add = (parent, tag, properties = {}) => Object.assign(parent.appendChild(document.createElement(tag)), properties);
    const root = add(document.body, "div", {id: "host-loop"});
    add(root, "style", {textContent: `
        #host-loop { position: fixed; inset: 0; margin: 0; padding: 0; overflow: hidden; background: #0b0d12; color: #f2f4f8;
                     font: 14px/1.4 system-ui, sans-serif; text-align: left; }
        #host-loop canvas { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: cover; }
        #host-loop .panel { position: absolute; margin: 0; background: rgba(14, 16, 22, .72); border: 1px solid rgba(255, 255, 255, .16);
                            border-radius: 14px; }
        #host-loop .controls { left: 50%; bottom: 20px; transform: translateX(-50%); display: flex; gap: 8px; align-items: center;
                               padding: 8px; white-space: nowrap; }
        #host-loop .stats { left: 20px; top: 20px; max-width: calc(100% - 230px); padding: 10px 14px; font: 11px/1.5 ui-monospace, monospace;
                            white-space: pre-wrap; }
        #host-loop .status { left: 50%; bottom: 84px; transform: translateX(-50%); padding: 8px 14px; color: #ffd27d; }
        #host-loop .status:empty { padding: 0; border: 0; }
        #host-loop .chip, #host-loop select, #host-loop button { font: inherit; color: inherit; background: rgba(255, 255, 255, .08);
            border: 1px solid transparent; border-radius: 9px; padding: 7px 12px; cursor: pointer; }
        #host-loop select { appearance: none; -webkit-appearance: none; }
        #host-loop select option { color: #111; background: #fff; }
        #host-loop .chip input { position: absolute; opacity: 0; pointer-events: none; }
        #host-loop .chip:has(input:checked) { background: #f2f4f8; color: #0b0d12; }
        #host-loop .chip:has(input:focus-visible), #host-loop select:focus-visible, #host-loop button:focus-visible { border-color: #9ecbff; outline: none; }
        #host-loop [name=stop] { background: rgba(255, 96, 96, .22); }
        #host-loop [name=cuda] { right: 20px; top: 20px; padding: 10px 16px; border-radius: 14px; text-align: right; background: rgba(14, 16, 22, .72);
                                 border: 2px solid rgba(255, 255, 255, .25); color: #9aa3b2; }
        #host-loop [name=cuda] b { display: block; font: 800 26px/1 system-ui, sans-serif; letter-spacing: 2px; }
        #host-loop [name=cuda] span { font-size: 12px; }
        #host-loop [name=cuda].on { background: #76b900; border-color: #b6f03c; color: #0b1400; }
        #host-loop [name=cuda]:disabled { cursor: not-allowed; opacity: .6; }
    `});
    const canvas = add(root, "canvas", {width: 960, height: 720});
    const stats = add(root, "pre", {className: "stats panel"});
    // The engine is one switch, large enough to be read on a capture: lit, CUDA computes; dark, the CPU does.
    const cuda = add(root, "button", {name: "cuda", className: "panel", disabled: true});
    add(cuda, "b", {textContent: "CUDA"});
    const cudaState = add(cuda, "span", {textContent: "unavailable"});
    const controls = add(root, "div", {className: "controls panel"});
    const quality = add(controls, "select", {name: "quality", title: "Image size and rays per pixel"});
    ["Low · 320 × 240", "Medium · 640 × 480", "High · 640 × 480, 4 rays", "Ultra · 960 × 720, 4 rays"]
        .forEach((textContent, value) => add(quality, "option", {value, textContent}));
    const chip = (text, name) => add(add(controls, "label", {className: "chip", textContent: text}), "input", {type: "checkbox", name});
    const glass = chip("Glass and beams", "glass");
    const paused = chip("Pause", "paused");
    const stop = add(controls, "button", {name: "stop", textContent: "Stop the host"});
    const status = add(root, "p", {className: "status panel"});
    status.setAttribute("role", "status");
    const context = canvas.getContext("2d");

    let device = "", cpuThreads = 0, wantCuda = false, stopping = false, sending = 0, settled = 0, requestedAt = 0;
    let framesPerSecond = 0, drawMs = 0, bridgeMs = 0;
    const shownAt = [];
    const captureMode = Boolean(globalThis.hostLoopCapture);
    let captureResolve = null;
    if (captureMode) glass.checked = Boolean(globalThis.hostLoopCapture.glass);
    const showEngine = () => {
        cuda.classList.toggle("on", wantCuda);
        cuda.setAttribute("aria-pressed", String(wantCuda));
        cudaState.textContent = !device ? "unavailable" : wantCuda ? `${device} · on` : `off · CPU, ${cpuThreads} threads`;
    };
    const fail = error => { if (!stopping) status.textContent = error.message; };
    const next = () => {
        requestedAt = performance.now();
        call("host.nextFrame", framesPerSecond, drawMs, bridgeMs).catch(fail);
    };
    // A refused choice (CUDA where there is none) is said, and the next image puts the controls back. An image computed
    // before the host took a choice must not: the controls follow only the images of that revision of the settings.
    const send = async () => {
        ++sending;
        status.textContent = "";
        try { settled = Math.max(settled, await call("host.settings", Number(wantCuda), paused.checked, glass.checked, Number(quality.value))); }
        catch (error) { fail(error); }
        --sending;
    };
    for (const control of [quality, glass, paused]) control.onchange = send;
    cuda.onclick = () => { wantCuda = !wantCuda; showEngine(); send(); };
    // The host may end before the answer arrives: say it now, and let a lost connection pass.
    stop.onclick = () => {
        stopping = true;
        status.textContent = "Stop requested: the host application ends its loop.";
        call("host.stop").catch(() => {});
    };

    globalThis.hostLoop = {
        async captureNext(makeGlass) {
            await captureReady;
            if (!captureMode || captureResolve) throw new Error("Capture is unavailable or already pending");
            if (glass.checked !== makeGlass) {
                glass.checked = makeGlass;
                await send();
            }
            return new Promise(resolve => { captureResolve = resolve; next(); });
        },
        onFrame(data, [sequence, width, height, rays, revision, packed], [usedEngine, isPaused, isGlass, usedQuality], [computeMs, copyMs, packMs]) {
            bridgeMs = Math.max(0, performance.now() - requestedAt - computeMs - copyMs - packMs);
            requestAnimationFrame(now => {
                const start = performance.now();
                if (canvas.width !== width || canvas.height !== height) Object.assign(canvas, {width, height});
                let pixels = data;
                if (packed) {
                    pixels = new Uint32Array(width * height);
                    for (let i = 0, at = 0; i < data.length; at += data[i], i += 2) pixels.fill(data[i + 1], at, at + data[i]);
                }
                context.putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer), width, height), 0, 0);
                drawMs = performance.now() - start;
                shownAt.push(now);
                while (shownAt[0] <= now - 1000) shownAt.shift();
                framesPerSecond = shownAt.length;
                if (!sending && revision >= settled) {
                    wantCuda = Boolean(usedEngine); quality.value = usedQuality;
                    glass.checked = isGlass; paused.checked = isPaused;
                    showEngine();
                }
                const fixed = value => value.toFixed(1).padStart(5);
                stats.textContent =
                    `image ${sequence}: ${width} x ${height}, ${rays} ray${rays > 1 ? "s" : ""} per pixel, ` +
                    `${usedEngine ? "CUDA" : `CPU, ${cpuThreads} threads`}, ` +
                    `${packed ? `sent as runs (${(100 * data.length / pixels.length).toFixed(0)} % of the pixels)` : "sent as pixels"}\n` +
                    `compute ${fixed(computeMs)} ms  copy ${fixed(copyMs)} ms  pack ${fixed(packMs)} ms  ` +
                    `bridge ${fixed(bridgeMs)} ms  draw ${fixed(drawMs)} ms\n` +
                    `displayed ${framesPerSecond} images per second`;
                if (captureMode) {
                    const resolve = captureResolve;
                    captureResolve = null;
                    if (resolve) resolve(sequence);
                } else if (!stopping) next();
            });
        }
    };
    const captureReady = webFront.ready.then(() => call("host.info")).then(([name, threads]) => {
        device = name;
        cpuThreads = threads;
        cuda.disabled = !device;
        showEngine();
        if (!captureMode) next();
    });
    captureReady.catch(fail);
})();
)js";

/// Registers the callbacks; the application adds `page` to the browsers it opens. `host` must outlive `app`, and its
/// loop must have returned before `app` is destroyed (a request holds the browser it will answer).
template <typename App>
void install(App& app, Host& host) {
    using BrowserUI = typename App::UI;
    app.cppFunction("host.info", [&host] {
        return std::tuple{host.device(), host.threads()};
    });
    app.cppFunction("host.settings", [&host](std::uint32_t engine, bool paused, bool glass, std::uint32_t quality) {
        return host.set({static_cast<Engine>(engine), paused, glass, quality});
    });
    app.cppFunction("host.nextFrame", [&host](BrowserUI ui, double framesPerSecond, double drawMs, double bridgeMs) {
        const auto send = [ui](const Frame& frame) {
            const auto& [engine, paused, glass, quality] = frame.settings;
            const bool packed                            = !frame.packed.empty();
            ui.jsFunction("hostLoop.onFrame")(
                std::span<const std::uint32_t>{packed ? frame.packed : frame.pixels},
                std::tuple{frame.sequence, frame.scene.width, frame.scene.height, frame.scene.samples * frame.scene.samples, frame.revision, packed},
                std::tuple{static_cast<std::uint32_t>(engine), paused, glass, quality},
                std::tuple{frame.computeMs, frame.copyMs, frame.packMs});
        };
        host.request(ui.id(), send, {framesPerSecond, drawMs, bridgeMs});
    });
    app.cppFunction("host.stop", [&host] {
        host.stop();
    });
}

}  // namespace boing
