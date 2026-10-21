# WebFront

[![CI](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml/badge.svg?branch=develop)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Windows · MSVC / WebView2](https://img.shields.io/badge/CI-Windows%20%C2%B7%20MSVC%20%2F%20WebView2-0078D4)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![macOS · Apple Clang / WKWebView](https://img.shields.io/badge/CI-macOS%20%C2%B7%20Apple%20Clang%20%2F%20WKWebView-555555)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Linux · GCC / WebKitGTK](https://img.shields.io/badge/CI-Linux%20%C2%B7%20GCC%20%2F%20WebKitGTK-E95420)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C)](CMakeLists.txt)
[![Header-only](https://img.shields.io/badge/library-header--only-2EA043)](include/)

**A cross-platform GUI for your C++ application, in a header-only C++23 library.**
Keep your logic in C++, build your interface with HTML/CSS and JavaScript, and connect them with ordinary callbacks. Start in your system browser; use WebView2, WKWebView or WebKitGTK for a native application window. WebFront is experimental; APIs and packaging are still evolving.

## Hello World

```cpp
#include <WebFront.hpp>

using App = webfront::WebFront;

int main() {
    App app;
    app.show();
}
```

That is the complete [Hello World](examples/hello-world/HelloWorld.cpp). The library serves its built-in welcome page and opens your browser. No HTML, Node.js or JavaScript build step is needed.

<a href="#game-of-life-from-console-to-native-application"><img src="docs/images/life-preview.jpg" width="480" alt="Game of Life: native window with an isometric grid, playback controls and generation counters"></a>

*Game of Life: C++ simulation, React interface, native window on Windows, macOS and Linux. [Build it step by step ↓](#game-of-life-from-console-to-native-application)*

You need Git, CMake **3.31+** and a compiler with C++23 support. Initial CMake configuration downloads the C++ dependencies.

```bash
git clone https://github.com/ambroise-leclerc/WebFront.git
cd WebFront
mkdir build
cd build
cmake ..
cmake --build . --parallel 2
./examples/HelloWorld
```

On Windows with Visual Studio, use `cmake --build . --config Release --parallel 2`, then `.\examples\Release\HelloWorld.exe`. Close the server by pressing Enter in the launching terminal.

## Game of Life: from console to native application

The example starts with a C++ simulation, adds a React interface, then embeds that interface in the executable. Node.js and npm are not required locally. The initial bootstrap and dependency installation require internet access.

The developer package for this 0.4 development branch is prepared but not yet published. For local review, use [local archive provisioning](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers).

### 1. Start with the C++ simulation

[Life.hpp](examples/life/Life.hpp) implements Conway's B3/S23 rules on a wrapping 32 × 32 grid. Each step creates a new `std::bitset<1024>`. [LifeConsole.cpp](examples/life/LifeConsole.cpp) runs it without a GUI:

```bash
# From the repository root:
./build/examples/WebFrontLifeConsole
```

On Windows the executable is `build\examples\Release\WebFrontLifeConsole.exe`.

### 2. Add a window and bootstrap the tools

The essential change in [LifeDeveloper.cpp](examples/life/LifeDeveloper.cpp) is:

```cpp
#include <WebFront.hpp>
#include "Life.hpp"

int main() {
    webfront::NativeApp app{"life-project"};
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.bootstrap("developer");
    app.show();
}
```

`NativeApp` selects your platform's native engine. The project directory explicitly grants access to development files. `bootstrap("developer")` loads a small embedded page, downloads the version-pinned developer environment, verifies its SHA-512 integrity and opens its terminal. Downloads are cached and checked again when reused. The terminal runs WebFront commands in the browser; it exposes no system shell.

Enable **one** engine for your platform:

| Platform | CMake option | Runtime |
| --- | --- | --- |
| Windows | `-DWEBFRONT_EMBED_WEBVIEW2=ON` | Microsoft Edge WebView2 Evergreen |
| macOS 13.3+ | `-DWEBFRONT_EMBED_WKWEBVIEW=ON` | System Cocoa / WebKit |
| Linux | `-DWEBFRONT_EMBED_WEBKITGTK=ON` | WebKitGTK 6.0; [installation](docs/WEBKITGTK.md) |

For example, on macOS, from the repository root:

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DWEBFRONT_EMBED_WKWEBVIEW=ON
cmake --build build-native --target WebFrontLifeDeveloper --parallel 2
./build-native/examples/WebFrontLifeDeveloper "$PWD/life-project"
```

Replace the engine option on Windows or Linux. With Visual Studio add `--config Release` when building and run `.\build-native\examples\Release\WebFrontLifeDeveloper.exe "$PWD/life-project"` in PowerShell. The example accepts the project directory as its argument, creates it if necessary, and reports startup failures with a nonzero exit status. Use a fresh project directory for this walkthrough.

### 3. Install dependencies in the application's terminal

In the downloaded terminal, run:

```text
wf create life
wf install react@19.3.1 react-dom@19.3.1
wf preview ui/main.tsx
```

`wf create life` writes the editable [TSX and CSS starter](examples/life/ui), without overwriting an existing project. `wf install` fetches the browser packages and their dependencies, verifies their integrity and records exact versions in `webfront-packages.lock.json`. It does not run npm installation scripts. `wf preview` compiles TSX with WebAssembly and opens the interface inside the application.

Edit `life-project/ui/main.tsx` and `style.css` with your usual editor, then run `wf preview` again. React renders the field and controls; each generation awaits the C++ callback:

```js
await webFront.ready;
const step = webFront.cppFunction('life.step');
const next = await step(cells);
```

`cells` and `next` are 128-byte `Uint8Array` grids mapped to C++ `std::bitset<1024>`, least-significant bit first in each byte. JavaScript calls always return a Promise: await them or handle rejection. No Life rules run in JavaScript.

### 4. Try the result

![Game of Life running in a native window](docs/images/life-preview.jpg)

Play or pause, advance one generation, paint cells, switch patterns, change speed or select a plan view. Generation and population counters reflect the C++ simulation. Only one generation request is outstanding at a time; failures stop playback and appear in the interface.

### 5. Embed the interface for distribution

In the application's terminal:

```text
wf embed ui/main.tsx --output generated/LifeUI.hpp
```

This builds the interface and packages its resources and dependency notices into a compressed C++ header. Close the development window, then reconfigure with the **same engine option** used above. On macOS:

```bash
cmake -S . -B build-native -DWEBFRONT_EMBED_WKWEBVIEW=ON \
  -DWEBFRONT_LIFE_ASSETS="$PWD/life-project/generated/LifeUI.hpp"
cmake --build build-native --target WebFrontLifeEmbedded --parallel 2
./build-native/examples/WebFrontLifeEmbedded
```

For Visual Studio add `--config Release` and launch `build-native\examples\Release\WebFrontLifeEmbedded.exe`.

[LifeEmbedded.cpp](examples/life/LifeEmbedded.cpp) replaces the development project and bootstrap with the generated provider:

```cpp
webfront::NativeAppWithAssets<webfront::fs::LifeUI> app;
app.cppFunction("life.step", &webfront::demo::nextLife);
app.show();
```

Recompilation is explicit. The resulting application runs offline without the project directory, developer callbacks or downloaded tool cache. The selected native engine's deployment requirements still apply; this is not an application installer.

## Use WebFront in your project

WebFront's implementation lives under `include/`. There is no WebFront library binary to build; CMake supplies its C++ dependencies and platform settings:

```cmake
add_subdirectory(path/to/WebFront)
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
```

As a subproject, examples and tests are off by default and compiler settings do not leak into your targets. `WEBFRONT_BUILD_EXAMPLES=OFF` disables examples in a standalone build too.

`webfront::WebFront` always uses the system browser. Native adapters are explicit choices and remain optional; CEF is available when you need an embedded Chromium distribution. See [WKWebView](docs/WKWEBVIEW.md), [WebKitGTK](docs/WEBKITGTK.md) and the [frontend architecture](docs/adr/README.md).

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Tests additionally require Node.js for offline JavaScript checks. Browser tests require an enabled engine; Linux CEF tests use Xvfb. See [AGENTS.md](AGENTS.md) for contributor commands and [developer environment testing](docs/DEVELOPER_ENVIRONMENT.md#environment-distribution-and-offline-tests-maintainers).

CI runs one combined C++ / native-browser job per desktop platform on each code change. Linux Clang, sanitizers, fuzzing, coverage and CEF compatibility run weekly and on demand. Workflow concurrency cancels superseded runs; dependency and compiler caches avoid repeated work. Platform badges above identify the configured matrix; the CI badge reports its aggregate status. Execution times depend on cache state and runner availability.

## Contracts and limitations

- WebFront owns an HTTP/WebSocket server on a background thread. Callbacks run there; avoid blocking it. Native windows must be created and shown on the main thread. Application coroutines use `app.spawn(task)` or `ui.spawn(task)`; see [execution and cancellation](docs/ASYNC_EXECUTION.md).
- JavaScript calls to C++ always return Promises. C++ `ui.jsFunction(name)(...)` is fire-and-forget; `ui.jsFunction<R>(name)(...)` returns `std::future<R>`. Missing functions, exceptions, malformed replies and disconnects fail the typed call.
- The binary bridge supports scalars, strings, tuples, numeric typed arrays and packed bitsets. Arbitrary objects do not cross it. Strings have a 16-bit encoded length limit.
- Development project access is explicit and intended for trusted local code. Downloaded tools run with the application's bridge authority. Package scripts, native Node add-ons and Node APIs are unsupported. TypeScript is transformed without type checking.
- Packaging, accessibility and native-engine deployment still need application-specific work. General DOM abstractions remain future work. See [project status](docs/PROJECT_STATUS.md) for known limitations.

[Developer environment reference](docs/DEVELOPER_ENVIRONMENT.md) · [UI components](docs/UI_COMPONENTS.md) · [architecture decisions](docs/adr/README.md) · [contributing](CONTRIBUTING.md)
