# <img src="docs/images/webfront-icon.svg" width="24" height="24" alt=""> WebFront

[![CI](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=develop)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Windows · MSVC](https://img.shields.io/badge/CI-Windows%20%C2%B7%20MSVC-0078D4)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![macOS · Apple Clang](https://img.shields.io/badge/CI-macOS%20%C2%B7%20Apple%20Clang-555555)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Linux · GCC](https://img.shields.io/badge/CI-Linux%20%C2%B7%20GCC-E95420)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Linux · aarch64](https://img.shields.io/badge/CI-Linux%20%C2%B7%20aarch64%20%C2%B7%20GCC%2013-E95420)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Jetson Orin](https://img.shields.io/badge/CI-Jetson%20Orin-76B900)](docs/JETSON.md)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C)](CMakeLists.txt)
[![Header-only](https://img.shields.io/badge/library-header--only-2EA043)](include/)

[English](README.md) · [Français](README.fr.md)

WebFront is a **header-only C++23 library for building rich, modern user interfaces.** Keep application logic in C++ and create reactive interfaces with TypeScript, React and the existing JavaScript package ecosystem. WebFront brings UI development tools into your application: there is no separate TypeScript development environment to install.

WebFront has no dependencies and can be deployed on any device with a recent C++ standard library.

<a href="#game-of-life-from-console-to-native-application"><img src="docs/images/life-bootstrap.gif" width="100%" alt="Game of Life walkthrough: the C++ application beside the WebFront window, where wf create life, dependency installation and wf preview bring the React interface up below the terminal"></a>

The animation shows the C++ application and the Game of Life example, with terminal output and timing condensed and the preview expanded for readability. [Follow the walkthrough below ↓](#game-of-life-from-console-to-native-application)

- **Integrate into an existing application.** Keep your simulation, device control, SIMD computations, CUDA kernels... and worker threads in C++. Use a native window or even a web browser without surrendering your event loop. [Host integration →](docs/HOST_INTEGRATION.md)
- **Support human developers and coding agents.** The integrated `wf` tools compile, type-check, test and embed the UI. An opt-in MCP server and JSON CLI let coding agents operate your application's own window and run permitted checks and callbacks. [Agent workflow →](#developing-with-an-agent)
- **Move numerical data directly.** Typed arrays and packed bitsets cross a binary WebSocket bridge without JSON or base64 conversion. Coroutines, futures and senders connect asynchronous UI calls to native work. [Execution model →](docs/ASYNC_EXECUTION.md)
- **Build for distribution.** Embed assets, pin package versions and verify downloads; run the resulting application offline. CMake consumer targets and browser tests support repeatable builds and CI. [Integrate →](#use-webfront-in-your-project) · [Test →](#test-your-application)
- **Choose your UI stack.** Use TypeScript, React and browser-compatible packages, or build DOM islands directly from C++. Optional forms and inspectors derive from C++ type descriptors. [DOM →](docs/DOM.md) · [React →](docs/REACT.md) · [Forms →](docs/FORMS.md)



**Website and book: [webfront.fr](https://webfront.fr/en/)** · [The WebFront Book](https://webfront.fr/en/book/) · [Documentation](docs/) · [Changelog](docs/CHANGELOG.md)

## Platforms and deployment

| Target | Interface options | Validation scope |
| --- | --- | --- |
| Windows | System browser, native WebView2, optional CEF | Desktop CI with MSVC; WebView2 needs the Evergreen runtime |
| macOS 13.3+ for the native window | System browser, system WKWebView, optional CEF | Desktop CI with AppleClang |
| Linux | System browser, native WebKitGTK 6.0, optional CEF | Desktop CI with GCC; extended Clang coverage |
| Linux ARM64 / NVIDIA Jetson | System browser, WebKitGTK or CEF where available | GCC 13 ARM64 CI and on-board **Orin Nano Super 8 GB** validation; see the [Jetson guide](docs/JETSON.md) |

Designed to follow standard C++ across platforms, the core uses standard facilities and a replaceable networking provider. The default browser path avoids linking a native GUI toolkit or web-view engine. A target needs a suitable **C++23 compiler and standard library, OS networking and threads, and a browser**. GCC 13 is the best-effort minimum; the [support policy](docs/adr/ADR-022-profils-et-niveaux-de-support.md) defines tested toolchains and support levels.

**No dependencies. No separate WebFront runtime.** The core follows the standard C++ networking and execution model. While these facilities are being standardized and adopted by standard libraries, CMake supplies pinned external implementations: the maintained `stdnet` networking layer and, when needed, `stdexec` for senders. These provide the interim implementation at build time; there is no WebFront library binary to install. Native web views retain their platform requirements, and CEF is optional. The developer environment caches verified tools and packages on first use; the embedded production build needs neither that cache nor the UI sources.

### CUDA and embedded applications

**Keep CUDA in your C++ application, including on NVIDIA Jetson Orin Nano.** WebFront does not prescribe the compute backend: your host loop or workers run CPU or CUDA code, while the web interface changes parameters and displays results through typed arrays. Keep `.cu` translation units separate from the C++23 WebFront headers when the CUDA compiler requires it. No CUDA toolkit is needed for applications that do not use CUDA.

**The included [Boing Ball example](docs/HOST_LOOP_EXAMPLE.md) runs on CPU or CUDA.** A checkered ball bounces above a reflective floor; switch to tinted glass to reveal reflection, refraction and coloured light beams. Every pixel is computed by the C++ host, while the page displays the resulting typed array on a canvas. The same host loop works with a native window or the system browser.

![CUDA Boing Ball animation in a native window on Jetson Orin Nano: checkered ball, then illuminated glass](docs/images/host-loop-boing.gif)

```bash
cmake --build build --target WebFrontHostLoop --parallel 2
build/examples/WebFrontHostLoop --quality 0
# On a CUDA-equipped build and device:
build/examples/WebFrontHostLoop --engine cuda --glass --stats
```

With a multi-configuration generator such as Visual Studio, replace `build/examples/` in the commands above with `build/examples/Debug/`, or `build/examples/Release/` after building with `--config Release`. The executable is named `WebFrontHostLoop.exe`.

Run these commands from the repository root after configuration. CUDA detection is automatic; `-DWEBFRONT_HOST_LOOP_CUDA=OFF` builds the complete CPU example without the toolkit. Add `--browser` to select the system browser. The [example guide](docs/HOST_LOOP_EXAMPLE.md) covers quality, benchmarking and the separate CUDA compiler configuration.

**Qualified on Jetson Orin Nano Super 8 GB:** real-board builds, CPU/CUDA rendering, native-window and bridge tests, and sandboxed distribution checks. A trusted [hardware validation workflow](docs/JETSON_RUNNER.md) complements hosted ARM64 CI. See the [board qualification](docs/JETSON.md) and [rendering measurements](docs/HOST_LOOP_EXAMPLE.md) for the tested software stack and scope. NX/AGX and other JetPack versions require their own qualification; accelerated rendering inside WebKit's sandbox is not established by these results.

On Linux, WebKitGTK defaults to development mode with its sandbox disabled. For distribution, select `-DWEBFRONT_DISTRIBUTION=ON` and package the application's sandbox profile where required. [Development and distribution →](docs/WEBKITGTK.md#development-and-distribution)

## Performance by design

Keep heavy computation native and send the data the interface needs: numeric buffers stay typed, bitsets stay packed, and bridge calls are asynchronous. The server has its own thread; your compute loop and workers remain independent. The optional React adapter coalesces C++ snapshots per browser, and the DOM API batches writes.

These mechanisms help control serialization, update frequency and UI overhead. End-to-end performance still depends on message size, rendering and the target hardware. Measure computation, transport and display separately; the [execution guide](docs/ASYNC_EXECUTION.md) explains where work runs and how to keep callbacks responsive.

For a measured example, the Boing Ball on **Orin Nano Super 8 GB at 25 W**, with a sandboxed WebKitGTK 2.52.6 window, displays:

| Scene | CPU (6 threads) | CUDA |
| --- | ---: | ---: |
| Opaque ball | 12 images/s | 21 images/s |
| Glass and light beams | About 0.6 images/s | 12 images/s |

Scene: 960 × 720, four rays per pixel; window: 800 × 600 CSS pixels. Release build, GCC 13.3, CUDA 13.2.86, L4T R39.2.1. These are board-specific observations, not universal throughput guarantees. The [measurement report](docs/HOST_LOOP_EXAMPLE.md#system-browser-and-distribution-follow-up) separates compute, copy, transport and display, and compares the system browser with the native window.

## Find your starting point

- **Evaluate the library:** [Hello World](#hello-world), then the [Game of Life walkthrough](#game-of-life-from-console-to-native-application).
- **Add a UI to existing software:** [CMake integration](#use-webfront-in-your-project) and the [host loop example](examples/host-loop/HostLoop.cpp).
- **Work with a coding agent:** [developer workflow](#developing-with-an-agent) and [application automation](#let-an-agent-drive-your-application).
- **Plan production:** [testing](#test-your-application), [contracts and limitations](#contracts-and-limitations) and [support status](docs/PROJECT_STATUS.md).

## Developing with an agent

If you are a coding agent, or work with one, read this before anything else. The [agent guide](docs/DEVELOPING_WITH_AGENTS.md) has the details.

- **The model.** A WebFront application is a C++ program whose interface is TypeScript/TSX. The interface lives in **`ui/`** of the project directory (`ui/main.ts` or `ui/main.tsx`) and is developed with **`wf dev`**, in the terminal that `app.bootstrap("developer")` opens in the application's window. Pages of hand-written HTML and classic JavaScript served through `documentRoot` are not the development path: none of the tools apply to them.
- **The sequence.** With the pinned developer environment: `wf create app` (the starter name, not your project's name), `wf init`, `wf install <package>` for browser dependencies, then `wf dev ui/main.ts`. Finish with `wf check`, `wf test` and `wf embed` using the project's configuration. Use `wf create life` for the React walkthrough. `wf help agents` prints the workflow.
- **An agent runs `wf` itself, in JSON or MCP.** Do not ask a person to type the commands. Configure with `-DWEBFRONT_BUILD_AUTOMATION=ON` to build `webfront-agent`; the application opts in with `webfront::automation::enableAutomation(app, {.grants = {...}, .listen = directory})`, whose grants stay the owner's; then:

```sh
webfront-agent --list                                              # the sessions shared with agents
webfront-agent --attach DIRECTORY --operation create --starter app
webfront-agent --attach DIRECTORY --operation preview --entry ui/main.ts   # after each edit, in place of wf dev
webfront-agent --print-mcp-config --attach DIRECTORY > .mcp.json   # the same operations as an MCP server
```

```json
{"mcpServers": {"webfront": {"command": "/path/to/webfront-agent", "args": ["--attach", "/path/to/directory", "--mcp"]}}}
```

In a project that embeds WebFront, `webfront_write_agent_guide()` writes these rules as `AGENTS.md` at its root, and `wf create` writes them beside the starter's sources.

## From C++ to a reactive UI

Register your simulation, bootstrap the tools, then build the interface in the application's terminal. `life-project` is the working directory for the UI sources:

<details>
<summary>The C++ application of the animation, as text</summary>

```cpp
#include <webfront/WebFront.hpp>
#include <bitset>

using namespace std;
constexpr int N = 32;
using Grid = bitset<N * N>;

Grid nextLife(const Grid& cells) {
    Grid next;
    auto index = [](int x, int y) {
        return size_t((y + N) % N * N + (x + N) % N);
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy)
                        n += cells[index(x + dx, y + dy)];
            auto i = index(x, y);
            next[i] = n == 3 || (cells[i] && n == 2);
        }
    return next;
}

int main() {
    webfront::NativeApp app{"life-project"};
    app.cppFunction("life.step", &nextLife);
    app.bootstrap("developer");
    app.show();
}
```

</details>

## Hello World

The smallest program, [HelloWorld.cpp](examples/hello-world/HelloWorld.cpp), serves the embedded page and opens it in your browser (`WebFront` always uses the system browser):

```cpp
#include <webfront/WebFront.hpp>

int main() {
    webfront::WebFront app;
    app.show();
}
```

```bash
mkdir build
cd build
cmake ..
cmake --build .
examples/HelloWorld
```

On Windows with Visual Studio run `examples\Debug\HelloWorld.exe` (`--config Release` selects `examples\Release\`). Press Enter in the terminal to stop the server.

The same application can build its page from C++ with the [DOM API](docs/DOM.md), with no UI files. Register the handler before `show()`, which returns only when the server stops:

```cpp
#include <webfront/WebFront.hpp>

int main() {
    webfront::WebFront app;
    app.onUIStarted([](auto ui) {
        auto doc = ui.document();
        auto root = doc.island(); // default: "root"
        auto button = doc.element("button");
        auto label = doc.text("Click me");
        button.append(label);
        button.listen("click", [label](std::string) { label.setText("Clicked"); });
        root.append(button);
    });
    app.show();
}
```

## Game of Life: from console to native application

The example starts with a C++ simulation, adds a React interface, then embeds that interface in the executable. You need Git, CMake **3.31+** and a C++23 compiler (GCC **13+**; GCC 13 has best-effort S2 support). See [build tools on LTS distributions](#build-tools-on-lts-distributions) when your distribution supplies an older CMake, and the [compiler support policy](docs/adr/ADR-022-profils-et-niveaux-de-support.md) for the tested toolchains and limits. The initial CMake configuration, the first bootstrap and the first `wf install` need internet access; afterwards the downloaded environment and packages are cached and can be verified offline.

```bash
git clone https://github.com/ambroise-leclerc/WebFront.git
cd WebFront
mkdir build
cd build
cmake ..
cmake --build .
```

No option is needed: the build enables this platform's native window engine when its dependencies are found (see [step 2](#2-add-a-window-and-bootstrap-the-tools)). The commands below start from this `build` directory. With a multi-configuration generator such as Visual Studio, executables are in `examples\Debug\` (`--config Release` selects `examples\Release\`).

### 1. Start with the C++ simulation

[Life.hpp](examples/life/Life.hpp) implements Conway's B3/S23 rules on a wrapping 32 × 32 grid. Each step creates a new `std::bitset<1024>`. [LifeConsole.cpp](examples/life/LifeConsole.cpp) runs it without a GUI:

```bash
examples/WebFrontLifeConsole
```

On Windows with Visual Studio run `examples\Debug\WebFrontLifeConsole.exe`.

### 2. Add a window and bootstrap the tools

The essential change in [LifeDeveloper.cpp](examples/life/LifeDeveloper.cpp) is:

```cpp
#include "LifeFrontend.hpp"

webfront::demo::LifeApp app{"life-project"};
app.cppFunction("life.step", &webfront::demo::nextLife);
app.bootstrap("developer");
app.show();
```

The project directory gives the application access to its development files. `bootstrap("developer")` opens the integrated terminal and loads the version-pinned developer environment, checking its SHA-512 integrity. Downloads are cached, so later launches work offline. The terminal runs `wf` commands on the project and has `ls`, `cat`, `tree` and `mounts` to look at it; no system shell is exposed. Tools and packages are cached for this environment, without a host JavaScript installation.

The build you made above is enough. Start the example from the `build` directory:

```bash
examples/WebFrontLifeDeveloper
```

On Windows with Visual Studio run `examples\Debug\WebFrontLifeDeveloper.exe`. Without argument, the example uses `life-project` in the current directory (`build/life-project`), creates it when it is missing and reuses an existing one without touching its files; every `life-project/...` path below is relative to it. The pinned environment is used.

**Native window.** At configuration, CMake enables the native engine of your platform when it finds its dependencies, with no option to pass:

| Platform | Engine | Detected when |
| --- | --- | --- |
| Windows | Microsoft Edge WebView2 Evergreen | an MSVC-compatible compiler is used (the SDK is downloaded; the Evergreen runtime is checked at start) |
| macOS 13.3+ | System Cocoa / WebKit | Clang and a deployment target of at least 13.3 |
| Linux | WebKitGTK 6.0 | `pkg-config webkitgtk-6.0` succeeds; [installation](docs/WEBKITGTK.md) |

On macOS, this opens a WKWebView window using the system Cocoa and WebKit frameworks, with no engine download.
The example constructs the application and calls `show()` on the main thread; the HTTP/WebSocket server runs on its own thread.
Closing the window stops the server and exits the example. An explicit deployment target below 13.3 disables automatic detection;
explicitly enabling WKWebView with such a target fails configuration. See [the macOS guide](docs/WKWEBVIEW.md).

When the engine is missing, configuration still succeeds. CMake prints a `Native window: no native engine will be built` notice that says what to install, and `WebFrontLifeDeveloper` is compiled for the default browser: at start it prints that notice, serves the same application and opens it in your system browser. Install the dependency and run `cmake ..` again to get the native window. CEF (`-DWEBFRONT_EMBED_CEF=ON`) makes `CEFFrontend` available to your own applications; the examples do not select it.

<details>
<summary>Advanced native engine and environment options</summary>

- Choose the engine yourself with `-DWEBFRONT_EMBED_WEBVIEW2=ON|OFF`, `-DWEBFRONT_EMBED_WKWEBVIEW=ON|OFF` or `-DWEBFRONT_EMBED_WEBKITGTK=ON|OFF`; an explicit value is never replaced by the detection. `-DWEBFRONT_AUTODETECT_NATIVE=OFF` disables it. Test builds (`-DWEBFRONT_ENABLE_TESTING=ON`) keep the explicit selection.
- Automatic engine choices are recalculated on each configuration and do not create an engine option in the cache. To return from an explicit macOS choice to automatic detection, run `cmake .. -UWEBFRONT_EMBED_WKWEBVIEW` (or remove the corresponding engine option on another platform).
- Another project directory, and another environment archive, can be passed: `examples/WebFrontLifeDeveloper <project> [<environment-url> <sha512-integrity>]`, for instance an environment [built from the sources](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers).

</details>

The example reports startup failures with a nonzero exit status.

### 3. Create the project and install its dependencies

In the application's terminal, run:

```text
wf create life
wf init
wf install react@19.3.1 react-dom@19.3.1
wf dev ui/main.tsx
```

- `wf create life` writes the editable [TSX and CSS starter](examples/life/ui) into `ui/`; it never overwrites an existing directory. The argument names a starter, not your project: `wf create app` writes a minimal TypeScript interface for a new application instead.
- `wf init` writes `webfront.project.json`, the manifest every other command reads: dependencies, entries, providers, scripts and permissions.
- `wf install` resolves the packages and their dependencies from the registry, verifies their integrity and records exact versions in `webfront-packages.lock.json`. It does not run the installation scripts of packages. `wf restore` rebuilds the cache from the lock on another machine, and `wf restore --offline` verifies it without any network.
- `wf dev` compiles the TSX with esbuild-wasm, opens the interface inside the application and rebuilds it whenever you save a source. A build error appears in the terminal and over the preview, which keeps its last good version. Ctrl+C stops it; `wf build` compiles once without opening anything.

Edit `life-project/ui/main.tsx` and `style.css` with your usual editor. React renders the field and controls; each generation awaits the C++ callback:

```js
await webFront.ready;
const step = webFront.cppFunction('life.step');
const next = await step(cells);
```

`cells` and `next` are 128-byte `Uint8Array` grids mapped to C++ `std::bitset<1024>`, least-significant bit first in each byte. JavaScript calls always return a Promise: await them or handle rejection. No Life rules run in JavaScript.

### 4. Run the tests and check the types

`wf test` runs your [Jasmine](https://jasmine.github.io/) specs in the application's window, under the `jasmine-core` your lock pins. Install it, create `life-project/tests/life.spec.js` and list it in `webfront.project.json` (`wf init` does not copy the starter's recipe, so the manifest only holds what you add):

```js
describe('the Life grid', () => {
    it('packs 32 by 32 cells into 128 bytes, least significant bit first', () => {
        const cells = new Uint8Array(32 * 32 / 8);
        for (const i of [330, 331, 332]) cells[i >> 3] |= 1 << (i & 7);
        expect(cells.length).toBe(128);
        expect(cells[41]).toBe(0b00011100);
    });
});
```

```text
wf install jasmine-core@7.0.2
```

```json
"test": {"specs": ["tests/life.spec.js"]}
```

```text
wf test
```

The command ends with a nonzero code when a spec fails.

The builds remove TypeScript types without checking them; `wf check` runs the TypeScript compiler that your lock pins, in the browser. It needs a `tsconfig.json` and the type packages of your imports. A complete one for the starter, the same as the journey tests use, is:

```json
{
  "compilerOptions": {
    "strict": true,
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "jsx": "react-jsx",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "noEmit": true
  },
  "include": ["ui"]
}
```

The type packages come next: `wf install typescript@5.9.3 @types/react@19.3.0 @types/react-dom@19.3.0` then `wf check` type-check the starter. Both commands are described in the [developer environment reference](docs/DEVELOPER_ENVIRONMENT.md).

### 5. Try the result

![Game of Life running in a native window](docs/images/life-preview.jpg)

Play or pause, advance one generation, paint cells, switch patterns, change speed or select a plan view. Generation and population counters reflect the C++ simulation. Only one generation request is outstanding at a time; failures stop playback and appear in the interface.

### 6. Embed the interface for distribution

Declare the entry and the provider in `webfront.project.json` (`"entries": {"main": "ui/main.tsx"}` and `"providers": {"LifeUI": {"entry": "main", "output": "generated/LifeUI.hpp"}}`), or pass them explicitly. In the application's terminal:

```text
wf embed ui/main.tsx --output generated/LifeUI.hpp
```

This builds the interface and packages its resources and dependency notices into a compressed C++ header. Close the development window, then reconfigure the same `build` directory (the engine is kept):

```bash
cmake .. -DWEBFRONT_LIFE_ASSETS="$PWD/life-project/generated/LifeUI.hpp"
cmake --build . --target WebFrontLifeEmbedded
examples/WebFrontLifeEmbedded
```

For Visual Studio launch `examples\Debug\WebFrontLifeEmbedded.exe`.

[LifeEmbedded.cpp](examples/life/LifeEmbedded.cpp) replaces the development project and bootstrap with the generated provider:

```cpp
#include "LifeFrontend.hpp"

webfront::demo::LifeAppWithAssets<webfront::fs::LifeUI> app;
app.cppFunction("life.step", &webfront::demo::nextLife);
app.show();
```

Recompilation is explicit. The resulting application runs offline without the project directory, developer callbacks or downloaded tool cache. The selected native engine's deployment requirements still apply; this is not an application installer.

The examples' [LifeFrontend.hpp](examples/life/LifeFrontend.hpp) selects `NativeOrBrowserFrontend` for both stages: a native window when the engine is built and a session exists, the default browser otherwise.
Your own application can select `webfront::NativeApp` or `webfront::NativeAppWithAssets` explicitly when it requires a native window.

## Let an agent drive your application

Your coding agent can type-check, build, preview, inspect and click the interface of **your own application**, in its own
window and with its own C++ callbacks, while you keep the window and decide what the agent may do. Nothing is on by default.

<img src="docs/images/automation-agent.svg" width="100%" alt="Animated transcript. On the left, an agent lists the shared session, checks, builds and previews the interface, clicks a button, calls the allow-listed C++ callback bf.render and is refused bf.secret. On the right, the application window shows the interface updating, and the C++ process logs only the callbacks that ran.">

*Abridged transcript of the `AutomationConsumerWindow` regression test, which runs these steps against a real application.*

One call in your C++ turns it on, and the grants are yours, fixed in native code:

```cpp
#include <webfront/automation/Enable.hpp>

webfront::NativeApp app{project};
app.bootstrap("developer");
app.cppFunction("bf.render", render);                  // your callbacks stay yours
auto automation = webfront::automation::enableAutomation(
    app, {.grants    = {"check", "build", "preview", "inspect", "interact", "call"},
          .callbacks = {"bf.render"},                  // the only callback an agent may call
          .listen    = privateEndpointDirectory});
app.show(automation->launchPage());
automation->close();
app.shutdown();
```

- **Visible to you.** The window's terminal prints the grants and how to attach, `wf help` says so, and the window title can carry a marker.
- **Found without being told.** `webfront-agent --list` shows the sessions their owners shared (never the capability), and `webfront-agent --print-mcp-config --attach <dir>` writes the `.mcp.json` for Claude Code or any MCP client. `wf create` adds an `AGENTS.md` that tells the agent how to attach.
- **Bounded.** Each operation is a separate grant; a callback is reachable only by an exact name you listed; a stuck renderer is stopped by native deadlines; project code still runs with your rights (this is not a sandbox).
- **Optional.** It lives in a separate launcher and one header; applications that do not call `enableAutomation` register nothing. `webfront_add_agent()` builds and installs the launcher for `add_subdirectory` and `FetchContent` consumers.

The [automation guide](docs/AUTOMATION.md#automating-your-own-application) covers grants, callbacks, discovery, deadlines and the MCP profile.

## Capabilities for production projects

Beyond the basic bridge, WebFront provides [owned application coroutines and cancellation](docs/ASYNC_EXECUTION.md), broadcasts to connected browsers, [C++ DOM islands](docs/DOM.md), [forms and TypeScript declarations](docs/FORMS.md), and the optional [React store and `Island` adapter](docs/REACT.md). Adopt these independently; React is not required by the core.

For release history, migration notes and breaking changes, see the [changelog](docs/CHANGELOG.md). The [project status](docs/PROJECT_STATUS.md) records current validation and limitations; it is the reference when selecting a platform or frontend for production.

## Build tools on LTS distributions

WebFront requires CMake **3.31+** for both standalone builds and `add_subdirectory`/CPM consumers, including builds with tests disabled. Ubuntu 22.04 supplies CMake 3.22.1 and Ubuntu 24.04 supplies 3.28.3, so install a newer CMake before configuring. Git 2.34.1, as supplied by Ubuntu 22.04, works with the repository's commit checks. The [tooling decision and audit](docs/adr/ADR-022-profils-et-niveaux-de-support.md#outils-de-construction-distributions-lts-527) explain the common CMake floor; optional mddlog has [separate requirements](docs/mddlog-integration.md).

One option is an isolated Python environment. On Ubuntu, install its prerequisite with `sudo apt-get install python3-venv` if needed, then run:

```bash
python3 -m venv "$HOME/.local/share/webfront-tools"
"$HOME/.local/share/webfront-tools/bin/python" -m pip install 'cmake>=3.31,<4'
export PATH="$HOME/.local/share/webfront-tools/bin:$PATH"
cmake --version
ctest --version
```

The CMake installation itself needs no root access. Run the `export PATH=...` line in each build shell (or add it to your shell profile); it selects the matching `cmake` and `ctest`. Then use the build commands below or the Game of Life walkthrough above. Alternatively, follow the [Kitware APT repository instructions](https://apt.kitware.com/) for your Ubuntu release. A newer CMake does not supply a newer C++ compiler: Ubuntu 22.04's default GCC 11 still needs replacing with a toolchain from the support policy.

## Use WebFront in your project

WebFront's implementation lives under `include/`. There is no WebFront library binary to build; CMake supplies its C++ dependencies and platform settings:

```cmake
add_subdirectory(path/to/WebFront)
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
```

As a subproject, examples and tests are off by default and compiler settings do not leak into your targets. `WEBFRONT_BUILD_EXAMPLES=OFF` disables examples in a standalone build too.

<details>
<summary>GCC 16 and C++ module scanning</summary>

GCC 16 crashes inside stdexec (`internal compiler error: Segmentation fault` in `__query.hpp`) when CMake scans C++ modules and adds `-fmodules-ts`, which happens by default for C++20 and later targets (CMP0155, Ninja). WebFront is header-only and needs no module scan. Turn it off for the targets that link WebFront and use no C++ modules, after creating them:

```cmake
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
webfront_disable_module_scan(MyApp)
```

Alternatively, `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)` before creating your targets does it for all of them; use it only if no target of your project uses C++ modules, since CMake then cannot order their compilation. Targets with source-level scan properties or `CXX_MODULES` file sets are still scanned.

Configuring WebFront as a subproject with GCC 16 and scanning enabled prints a warning naming each affected target, with these workarounds.

</details>

`webfront::WebFront` always uses the system browser. For a native window with the same browser fallback as the Life examples, add this after linking your target (also works after `CPMAddPackage`):

```cmake
webfront_use_native_window(MyApp)
```

Select the application type in `main.cpp` using the definition supplied by the helper:

```cpp
#include <webfront/WebFront.hpp>

#if defined(WEBFRONT_NATIVE_WINDOW)
using App = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
#else
using App = webfront::WebFront;
#endif

int main() {
    App app;
    app.show();
}
```

To get a native window when one can be opened and the system browser otherwise (for example over SSH, in a container or in CI, where no display exists), use `WebFrontWithFrontend<webfront::frontend::NativeOrBrowserFrontend>`: no `#if` is needed, and the reason for a fallback is printed once. See [docs/FRONTENDS.md](docs/FRONTENDS.md).

Native engines are detected by default, except in WebFront's own test and fuzzing builds (`WEBFRONT_ENABLE_TESTING=ON` or `WEBFRONT_ENABLE_FUZZING=ON`); a parent project's own `ENABLE_TESTING` or `ENABLE_FUZZING` affects neither the detection nor WebFront's tests. The helper applies the native engine's target setup and defines `WEBFRONT_NATIVE_WINDOW` only when that engine is enabled; otherwise CMake prints installation advice and the application uses the default browser. On Windows, detection requires an MSVC-compatible compiler and downloads the pinned WebView2 SDK; the Evergreen runtime must be installed separately. Linux needs WebKitGTK 6.0 development files, and macOS needs Clang and macOS 13.3 or later. Explicit `WEBFRONT_EMBED_<ENGINE>=ON|OFF` values take precedence; `WEBFRONT_AUTODETECT_NATIVE=OFF` disables detection. No option needs to be set before `add_subdirectory`/CPM for the default path.

For explicit engine selection, `webfront_native_frontend_option(out)` and `webfront_setup_frontends_for_target(MyApp)` are also callable from the parent or other directories. The latter applies setup for all enabled engines; the native helper selects only the platform's native engine and never CEF. See [WKWebView](docs/WKWEBVIEW.md), [WebKitGTK](docs/WEBKITGTK.md) and the [frontend architecture](docs/adr/README.md).

## Test your application

The optional [Jasmine and CTest integration](docs/CONSUMER_TESTING.md) runs browser specs against your application's page. Specs can capture PNG screenshots and image sequences, and can run native programs explicitly authorized by the test owner with `PROGRAMS`, using private temporary spaces for test files. The native-program browser integration is validated on Linux with CEF; see the guide for platform coverage and limitations. Authorized programs run with the user's permissions, so use trusted test code.

To select an embedded engine explicitly, `webfront::embeddedFrontendAvailable` reports whether one is compiled in and `webfront::EmbeddedWebFront` selects it. See [frontend selection](docs/FRONTENDS.md).

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 4
```

Configuring with tests enabled downloads, and verifies against their SHA-512, the pinned bootstrap environment and the packages of the test registry; the embedded assets are generated and checked by CMake alone (the `webfront-embedded-assets` target regenerates them). Normal C++ builds use versioned headers. Browser tests require an enabled engine; Linux CEF tests use Xvfb. See [AGENTS.md](AGENTS.md) for contributor commands and [developer environment testing](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers).

CTest runs up to four tasks at a time; independent driver sessions use OS-assigned ports, while
shared projects and nested builds retain resource locks.
See [test timings and parallelism](docs/TEST_PERFORMANCE.md) for measurements and constraints.
`ctest -L fast` runs the unit tests in about a second; [test tiers](docs/TEST_TIERS.md) describes what CI runs on each pull request, weekly and before a merge into `master`.

CI exercises C++ and native-browser paths on Windows, macOS and Linux. Extended jobs cover Clang, sanitizers, fuzzing, CEF and Linux ARM64, including ARM64 CEF/WebKitGTK browser runs. The [test tiers](docs/TEST_TIERS.md) define the exact schedule and required gates; the [workflow](.github/workflows/BuildAndTest.yml) records toolchains and options. Hosted ARM64 coverage and [real Jetson board qualification](docs/JETSON.md) are separate evidence. Platform badges identify the configured matrix; the CI badge reports its aggregate status.

## Contracts and limitations

- WebFront owns an HTTP/WebSocket server on a background thread. Callbacks run there, one at a time; avoid blocking it. Browser calls (`jsFunction`, `app.broadcast(name)(...)`) can be made from any thread and never wait for it; see [threads](docs/ASYNC_EXECUTION.md#threads). Most native windows (WKWebView, WebKitGTK, CEF) must be created and shown on the main thread; WebView2 accepts a secondary thread. An application that keeps its own loop uses `app.start()` (system browser, returns at once), `app.requestClose()` or `webfront::UIThread`; see [host integration](docs/HOST_INTEGRATION.md). Application coroutines use `app.spawn(task)` or `ui.spawn(task)`; see [execution and cancellation](docs/ASYNC_EXECUTION.md).
- JavaScript calls to C++ always return Promises. C++ `ui.jsFunction(name)(...)` is fire-and-forget; `ui.jsFunction<R>(name)(...)` returns `std::future<R>`. Missing functions, exceptions, malformed replies and disconnects fail the typed call.
- The binary bridge supports scalars, strings, tuples, numeric typed arrays, packed bitsets and homogeneous lists of supported compound values. Arbitrary objects do not cross it. Strings and messages are bounded by the message-size policy (64 MiB by default); strings of 65,536 bytes or more use the long-string encoding. See [the bridge contract](docs/adr/ADR-003-protocole-binaire-et-bornes.md).
- Development project access is explicit and intended for trusted local code. Downloaded tools run with the application's bridge authority, and script permissions frame only the `context` API: neither is a sandbox. Only packages that run in a browser are supported; installation scripts and native add-ons are not executed. Builds transform TypeScript without type checking; `wf check` type-checks with the compiler the project locks.
- An opt-in [native pseudoterminal](docs/PTY.md) connects an application-selected program to xterm.js on Linux, macOS and Windows.
- The optional [`@webfront/react` adapter](docs/REACT.md) supplies coalesced C++ stores and empty React islands. `WebFrontLifeReact` demonstrates a C++ panel within the Game of Life React UI; its package archive is prepared locally for registry publication.
- Embedded Linux has ARM64 CI coverage and specific Orin Nano board evidence; other boards, bare-metal targets and microcontrollers are not generally qualified. Check C++23, memory, networking and browser requirements for your target. The default local transport supplies neither TLS nor client authentication; remote deployment needs an application-specific security design.
- Packaging, accessibility and native-engine deployment still need application-specific work. The optional [`ui.document()` API](docs/DOM.md) provides DOM islands, automatically batched writes, event callbacks and grouped sender reads. See [project status](docs/PROJECT_STATUS.md) for known limitations.

## License

WebFront is dual-licensed under **Apache-2.0 OR CECILL-B**, at your option.
You may use, modify and redistribute WebFront under either license; you do not
need to comply with both simultaneously. See [LICENSE](LICENSE) for the full
Apache 2.0 and CeCILL-B version 1 texts, including the French and English
versions of CeCILL-B.

Third-party components retain their own licenses and notices. The WebFront
license choice does not replace the licenses of its dependencies or bundled
third-party code.

[Website](https://webfront.fr/en/) · [Book](https://webfront.fr/en/book/) · [Developer environment reference](docs/DEVELOPER_ENVIRONMENT.md) · [architecture decisions](docs/adr/README.md) · [contributing](.github/CONTRIBUTING.md)
