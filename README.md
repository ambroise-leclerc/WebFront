# WebFront

**[Version 0.2.0](https://github.com/ambroise-leclerc/WebFront/releases/tag/v0.2.0)** adds optional native windows on every desktop platform: WebView2 on Windows, WKWebView on macOS and WebKitGTK on Linux, with the explicit `NativeWebViewFrontend` alias. The system browser remains the default. This release also includes asynchronous bridge results, owned application coroutines, embedded UI components and input-decoding hardening.

**A straightforward way to add a cross-platform GUI to your C++ application.**

WebFront is a header-only C++23 graphical user interface library for Windows, Linux and macOS. Keep your application logic in C++, design the interface with HTML and CSS, and connect the two with ordinary C++ callbacks. Start with a few lines, then build the interface your application needs.

Your application keeps its own types and ownership model. There is no required base class for application objects, no meta-object compiler and no application-wide event loop to adopt. WebFront runs its server on a background thread; the chosen frontend handles displaying the interface. [How it fits your application](#how-it-fits-your-application) explains the threading and lifetime details.

![Signal Studio running in a native Windows window with WebView2: React controls and a waveform generated in C++](docs/images/signal-studio-webview2.png)

*Signal Studio, running in a native WebView2 application window. C++ generates the samples; the embedded React draws the waveform and provides the controls. No browser tabs, CDN or JavaScript build step. [Try the example](#signal-studio-the-react-example).*

Use your existing browser to get started, or choose an embedded application window. The simplest example needs no HTML, JavaScript, Node.js, bundler or separately managed web server.

**WebFront is experimental.** The examples work and the bridge is tested across platforms, but APIs and packaging are still developing. Read the [limitations](#limitations) before choosing it for a released product.

[Get started](#hello-world) · [Connect your interface to C++](#connect-your-interface-to-c) · [Add it to your project](#use-webfront-in-your-project) · [Choose a window](#choose-how-your-interface-opens)

## Hello World

```cpp
#include <WebFront.hpp>

using App = webfront::WebFront;

int main() {
    App app;
    app.show();
}
```

That is the complete program (`src/HelloWorld.cpp`). Build it with the classic CMake workflow; it needs no options and downloads neither Chromium nor a test framework:

```bash
git clone https://github.com/ambroise-leclerc/WebFront.git
cd WebFront
git checkout v0.2.0
mkdir build
cd build
cmake ..
cmake --build .
```

Then launch it from the build directory:

```bash
./src/HelloWorld
```

Your system browser opens the interface below. The page is included in the library, so there are no assets to prepare. **Connected to WebFront** confirms that the interface has connected to your C++ application. Press Enter in the terminal to stop the server.

![The embedded Hello, WebFront! page showing the connected indicator](docs/images/hello-world.png)

## Browser-powered development (experimental)

The optional [developer environment](docs/DEVELOPER_ENVIRONMENT.md) downloads its terminal and
JavaScript/WASM build tools on demand. Install browser libraries with `wf install`, preview local
TSX, then generate compressed C++ resource headers with `wf embed`. The Game of Life example pairs a small C++23 bitset simulation with a spatial React interface,
then demonstrates
recompiling an offline native application without installing Node.js or npm on the developer's machine.
The [version-pinned environment and starters](https://ambroise-leclerc.github.io/WebFront/)
are hosted on GitHub Pages; see the guide for the complete walkthrough.

## Connect your interface to C++

For a small first example, the Greeter connects a name field and a button to application code. Register an ordinary C++ lambda; WebFront deduces its signature:

```cpp
App app{webfront::WebFrontConfig{.documentRoot = "src/greeter"}};

app.cppFunction("greet", [](std::string name) {
    return "Hello, " + name + "!";
});

app.show();
```

The document root is an explicit, development-only opt-in: WebFront then serves the files of that directory (here `src/greeter/index.html`) instead of the fallback page. Without a document root WebFront performs no local file access at all.

The page is plain HTML plus one native JavaScript module (`src/greeter/greeter.mjs`). It waits for the bridge, then awaits the Promise that `cppFunction` returns:

```js
await webFront.ready;                         // resolves after the WebSocket handshake
const greet = webFront.cppFunction("greet");
const message = await greet("Ada");           // "Hello, Ada!" computed in C++
result.textContent = message;
```

Run it from the build directory (the build copies the assets to `src/greeter`):

```bash
./src/WebFrontGreeter
```

Exceptions thrown by the C++ callback reject the JavaScript Promise. `webFront.ready` rejects if the initial connection fails or closes; it does not report later disconnects.

Callback signatures are deduced for ordinary and `mutable` lambdas, function pointers and `std::function`. Generic lambdas and overloaded call operators cannot be deduced; register those with `app.cppFunction<ReturnType, Args...>(name, callable)`.

## How it fits your application

Keep calculations, data structures and resource ownership in C++. The interface calls the functions you register, and C++ can call back into the interface. There is no requirement to make your application objects inherit from a widget hierarchy or adopt a parent-owned object model.

The connection is asynchronous: interface code awaits a Promise when it calls C++, and C++ can request a result through `std::future`. WebFront owns its server thread and cleans it up when the application object is destroyed. Callbacks run on that server thread; synchronise access to shared application state and hand lengthy work to your own workers.

`show()` starts the server and opens the interface. It is a blocking call: the default frontend waits for Enter in the terminal, while an embedded frontend runs its window loop until the window closes. Embedded frontends also have platform thread requirements; WebView2 normally belongs on the main thread. You do not write a GUI event loop, but you still need to choose where `show()` belongs in your application's lifecycle.

For a command-line tool, this can mean adding a graphical control panel around existing functions. For an interactive application, it gives you HTML and CSS for layout while your C++ code continues to own the work. See [Bridge basics](#bridge-basics) for return values, errors and connection handling.

## A terminal for your application

`WebFrontTerminal` adds an interactive terminal to your application's interface. Its embedded [xterm.js](https://xtermjs.org/) line editor provides history and Tab completion, and sends each line to commands registered in C++. It is the first optional UI component; it needs no document root or internet access at run time.

```cpp
#include <components/Terminal.hpp>

webfront::TerminalApp app;
webfront::Terminal terminal{app, {.prompt = "demo> "}};
terminal.command("greet", "Greet someone", [](webfront::CommandArgs args) {
    return "Hello, " + (args.empty() ? std::string{"world"} : args[0]) + "!";
});
app.show(terminal.page);
```

A command's return value (or what it writes to a `std::ostream&` parameter) is printed; an exception is shown as an error. `help` and `clear` are built in. Output appears when the command finishes: there is no streaming, and Ctrl+C interrupts only browser commands. See [docs/UI_COMPONENTS.md](docs/UI_COMPONENTS.md) for the component architecture and the `wf` tool manager.

```bash
./src/WebFrontTerminal
```

`WebFrontTools` (`src/Tools.cpp`) adds the `wf` tool manager to the terminal: `wf search`, `wf install <package>[@version]`, `wf list`, `wf open` and `wf remove` manage web tools published on npm with the `webfront-tool` keyword. The page downloads them from registry.npmjs.org, checks their SHA-512 integrity and stores them in the working directory (`.webfront/tools/`, recorded in `webfront-tools.lock.json`). An installed tool can call every registered C++ function: install only tools you trust, during development.

## Choose how your interface opens

The application code and interface assets stay the same whichever frontend you select.

| Frontend | Platforms | What you need |
| --- | --- | --- |
| System browser (default) | Windows, Linux, macOS | An installed browser; no embedded engine download |
| Embedded CEF window | Windows, Linux, macOS | Enable CEF at configuration time; a large download |
| Embedded WebView2 window | Windows | Enable the SDK and install the Evergreen Runtime |
| Embedded WKWebView window | macOS 13.3+ | Enable Cocoa/WebKit support; the engine ships with macOS |
| Embedded WebKitGTK window | Linux | Install WebKitGTK 6.0 development packages and enable the option |
| Native web view alias (opt-in) | Windows, macOS, Linux | Enable the platform adapter above; select `NativeWebViewFrontend` |
| Embedded WebKitGTK window | Linux | Install WebKitGTK 6.0 (GTK 4) development packages and enable the option |

WebView2 uses Chromium; WKWebView and WebKitGTK use WebKit. Rendering can differ between
platforms. Choose CEF when you need the same engine across desktop platforms. Each embedded
frontend is opt-in; `webfront::WebFront` keeps the system browser as its default.

Construct the application and call `show()` on the main thread for embedded frontends. Their `open()`
blocks until window closure; `show()` then stops and joins the server. A `keepServerRunning` frontend
leaves the server active after opening returns; the system-browser adapter waits for Enter first.
If opening throws, `show()` stops and joins the server before propagating the exception. Unavailable
frontend types remain declared and throw a diagnostic naming the required CMake option.

The [shared content policy](docs/CONTENT_POLICY.md) restricts bridge connections to the application origin,
limits embedded navigation to that origin and blocks embedded popups by default. Explicit allow-lists and
development opt-outs are available. See the [frontend contract](docs/FRONTENDS.md#the-contract) for initialization and threading.

### Embedded window with CEF (opt-in)

By default WebFront opens your system browser. To get an application window instead, enable the Chromium Embedded Framework when configuring. CEF is a **large download** and is never fetched unless you ask for it:

```bash
cmake -DWEBFRONT_EMBED_CEF=ON ..
```

`WEBFRONT_EMBED_CEF` only makes CEF *available*; the frontend is chosen in your source by changing the alias, nothing else:

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
```

Selecting `CEFFrontend` in a build without CEF fails with a clear error. An embedded window stops the server when it closes. `webfront::WebFront` always uses the system browser, even in a CEF-enabled build.

### Embedded window with WebKitGTK (Linux, opt-in)

On supported Ubuntu/Debian systems, install the development package and pkg-config:

```bash
sudo apt install libwebkitgtk-6.0-dev pkg-config
```

Configure with `-DWEBFRONT_EMBED_WEBKITGTK=ON`, then select:

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::WebKitGTKFrontend>;
App app;
app.show();
```

Construct and open on the process main thread with a working X11 or Wayland display.
The window uses the system WebKitGTK 6.0 / GTK 4 runtime and returns when closed, including
pages that never connect to the bridge. GTK headers do not propagate through
`WebFront.hpp`. Each executable has persistent data/cache directories under the XDG roots,
restricted to `0700`; processes launched from the same executable share that profile.
Automated coverage uses X11/Xvfb; native Wayland and WSLg still need desktop qualification.
See [WebKitGTK setup, profiles and tests](docs/WEBKITGTK.md) for other distributions and runtime requirements.

To run the React demo in this window from the repository root:

```bash
cmake -S . -B build-webkitgtk -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WEBKITGTK=ON \
  -DWEBFRONT_DEMO_FRONTEND=WebKitGTK
cmake --build build-webkitgtk --target WebFrontApp --parallel
./build-webkitgtk/src/WebFrontApp react.html
```

### Embedded window with WebView2 (Windows, opt-in)

Configure with `-DWEBFRONT_EMBED_WEBVIEW2=ON`, then select the frontend explicitly:

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::WebView2Frontend>;
App app;
app.show(); // returns when the window closes and the server has stopped
```

CMake fetches Microsoft.Web.WebView2 SDK **1.0.3537.50**, verifies its SHA-256 and links the static loader; no machine-wide SDK or loader DLL is required. The option defaults to `OFF` and requires Windows with an MSVC-compatible compiler (MSVC or clang-cl); MinGW is rejected at configuration time. The loader follows the compiler's x86, x64 or ARM64 target, including when it differs from the host; unsupported targets fail during configuration. It never changes `webfront::WebFront`'s system-browser default. Selecting the adapter without support throws an error explaining how to enable it.

The separately installed **Microsoft Edge WebView2 Evergreen Runtime** is required. Follow the shared main-thread contract; WebView2 additionally requires STA-compatible COM. The window handles resizing and keyboard focus, and closing it stops the server.

This frontend applies the [shared content policy](docs/CONTENT_POLICY.md); DevTools and context menus remain available by default. See the [WebView2 frontend notes](docs/WEBVIEW2.md) for runtime distribution, configurable startup deadlines and profiles, DPI and the navigation policy.

### Embedded window with WKWebView (macOS, opt-in)

On macOS 13.3 or newer, configure with `-DWEBFRONT_EMBED_WKWEBVIEW=ON` and select:

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::WKWebViewFrontend>;
App app;
app.show(); // returns after the window closes and the server stops
```

The system supplies WebKit; there is no engine download or runtime installation. Construct and
open on the main thread. Each opening uses a non-persistent website data store. Quit and
`window.close()` return control to C++. The module demo selects this adapter with
`-DWEBFRONT_DEMO_FRONTEND=WKWebView`. See [WKWebView notes](docs/WKWEBVIEW.md) for lifecycle,
inspection, content-policy limits and bundled-executable networking.

## Use WebFront in your project

```cmake
cmake_minimum_required(VERSION 3.31)
project(MyApp LANGUAGES CXX)

add_subdirectory(WebFront)              # e.g. a git submodule or a vendored checkout
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)
```

When WebFront is not the top-level project it builds neither its examples nor its tests (opt in with `-DWEBFRONT_BUILD_EXAMPLES=ON` or `-DENABLE_TESTING=ON`). Dependencies are fetched with CPM during the first configure, so that step needs network access. There is no installable CMake package yet.

## Limitations

- WebFront is an experimental foundation. APIs and the wire format may change.
- Gzip fallback to identity runs synchronously on the server thread with a configurable 4 MiB default decoded-byte budget, including HEAD. See the [fallback budget](docs/HTTP_ASSETS.md#fallback-budget); decoded-asset caching and byte-wise reading remain follow-ups.
- Calls are asynchronous. Results cross the bridge as `std::future` (C++ calling JavaScript, `ui.jsFunction<R>(name)`) and Promises (JavaScript calling C++, `webFront.cppFunction(name)`), limited to scalars, strings (up to 65535 bytes each), tuples, numeric typed arrays and packed `std::bitset<N>` values. Richer objects and a DOM abstraction are out of scope.
- A configured document root exposes local files for **development only**; it is not a sandbox or a production packaging format.
- Nothing authenticates the browser: the server binds to loopback by default, but any local process can connect.
- With WebView2 enabled, `WebFront.hpp` exposes Windows SDK, Shell and WRL global names, including `::byte`. Qualify `std::byte` instead of relying on `using namespace std;`.
- The tested toolchains are listed in the CI workflow; other compilers may not work yet.

## Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `WEBFRONT_EMBED_CEF` | `OFF` | Download and link CEF so `CEFFrontend` is available (Windows, Linux and macOS only). Other platforms fail at configure time |
| `WEBFRONT_EMBED_WEBKITGTK` | `OFF` | Enable the system WebKitGTK 6.0 embedded frontend (Linux only) |
| `WEBFRONT_EMBED_WEBVIEW2` | `OFF` | Download the pinned WebView2 SDK and link its static loader (Windows only) |
| `WEBFRONT_EMBED_WKWEBVIEW` | `OFF` | Link the system Cocoa and WebKit frameworks for WKWebViewFrontend (macOS 13.3+ only) |
| `WEBFRONT_DEMO_FRONTEND` | `DefaultBrowser` | Frontend for `WebFrontApp`: `DefaultBrowser`, `CEF`, `WebView2`, `WKWebView`, `WebKitGTK` or `Native` (enable the matching platform adapter) |
| `WEBFRONT_WEBTEST_FRONTEND` | Initial cache value: first enabled embedded frontend for the platform; `CEF` if none | Embedded frontend for the shared Jasmine and lifecycle runners; WebView2 takes priority over CEF on Windows, WKWebView on macOS, WebKitGTK on Linux; explicit selection overrides the default |
| `ENABLE_TESTING` | `OFF` | Build the Catch2 suite and the Jasmine browser test |
| `WEBFRONT_BUILD_EXAMPLES` | `ON` when top-level | Build `HelloWorld`, `WebFrontGreeter` and `WebFrontApp` |
| `ENABLE_COVERAGE` | `OFF` | Instrument the C++ tests (GCC/Clang) |
| `ENABLE_SANITIZER_ADDRESS` / `ENABLE_SANITIZER_UNDEFINED_BEHAVIOR` | `OFF` | Enable ASan / UBSan on internal targets (GCC/Clang) |
| `ENABLE_SANITIZER_THREAD` | `OFF` | Enable TSan separately from ASan (GCC/Clang) |
| `ENABLE_FUZZING` | `OFF` | Build three libFuzzer decoders with ASan+UBSan (Clang with compiler-rt/libFuzzer; Linux CI) |

Frontend selectors keep their cached value on reconfiguration, even when another engine is enabled later. On Windows, use `-DWEBFRONT_WEBTEST_FRONTEND=WebView2` to switch an existing CEF runner, or `-UWEBFRONT_WEBTEST_FRONTEND` to recompute the default. CEF browser tests are validated only on Linux/Xvfb; selecting CEF on Windows or macOS builds the runner but emits a warning and registers no browser test.

Test targets automatically enable the vendor's hardened standard library: `_GLIBCXX_ASSERTIONS`
with libstdc++, `_LIBCPP_HARDENING_MODE_EXTENSIVE` with libc++, or `_MSVC_STL_HARDENING=1`
with MSVC STL. Supporting GCC/Clang compilers also use `-ftrivial-auto-var-init=pattern`;
older compilers emit a warning and retain the other hardening settings. These settings,
sanitizers and fuzz instrumentation are private to internal targets; linking `WebFront::WebFront`
does not impose them on a consumer.

For ASan+UBSan, add `-DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON`
to the configure command in [Contributing: build and test](#contributing-build-and-test). For a separate TSan build use
`-DENABLE_SANITIZER_THREAD=ON`. CI runs both configurations on the full CEF-off suite (libstdc++ for ASan+UBSan,
libc++ for TSan, which also exercises retry after a throwing `std::call_once` callback);
`UBSAN_OPTIONS=halt_on_error=1` makes a UBSan diagnostic fail the run.

Fuzzing requires a Clang distribution with the compiler-rt libFuzzer, ASan and UBSan runtimes.
Linux is the CI-validated configuration. Apple Clang does not ship libFuzzer: macOS requires
an external installation such as LLVM with compiler-rt, and compatible sanitizer runtimes.
CMake checks that these runtimes link before adding the fuzz targets.

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_BUILD_EXAMPLES=OFF -DENABLE_FUZZING=ON
cmake --build build-fuzz --parallel
# Repeat with http and websocket. Corpora are copied into the build tree.
./build-fuzz/fuzz/fuzz_messages build-fuzz/fuzz/corpus/messages \
  -max_total_time=30 -timeout=5 -max_len=65536 -rss_limit_mb=1024
```

Fuzzers cover bridge headers and every owning parameter family, incremental HTTP parsing,
and WebSocket headers/payloads with complete and chunked input. The checked-in seed corpus
comes from unit-test examples; CI runs each decoder for 30 seconds and uploads crash inputs
on failure. This is bounded smoke coverage, not an exhaustive security audit.

## Contributing: build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The automated real-browser tests run on Linux with Xvfb:

```bash
cmake -S . -B build-cef -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=ON -DENABLE_TESTING=ON
cmake --build build-cef --target webtest --parallel
ctest --test-dir build-cef -L web-integration --output-on-failure
```

The asset freshness checks require Node.js 22.18+ (24 in CI); ordinary C++ consumer builds do not require Node.js or npm.

The Jasmine specs are native ES modules. They prove relative module loading, both bridge directions, all supported numeric typed arrays and `webFront.ready`; report the final result to C++; close the embedded window automatically; and fail CTest on an assertion, bridge, startup or timeout error. CTest also checks that the commands and snippets in this README match the built examples.

The shared lifecycle suite checks closure without a bridge, immediate page closure, a rejected
`webFront.ready`, failed initial navigation, encoded filenames, blocked navigation/popups and an explicit
navigation allow-list. It runs with the same selected
frontend as Jasmine; every case has a timeout and the `web-integration` label. See
[frontend lifecycle coverage](docs/FRONTENDS.md#lifecycle-tests) for details.

On Linux, WebKitGTK runs the same Jasmine and shared lifecycle suites plus adapter-specific cases.
Install the WebKitGTK development package, pkg-config and Xvfb, then configure explicitly:

```bash
sudo apt install libwebkitgtk-6.0-dev pkg-config xvfb
cmake -S . -B build-webkitgtk -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_EMBED_WEBKITGTK=ON -DENABLE_TESTING=ON \
  -DWEBFRONT_DEMO_FRONTEND=WebKitGTK -DWEBFRONT_WEBTEST_FRONTEND=WebKitGTK
cmake --build build-webkitgtk --parallel
ctest --test-dir build-webkitgtk -L web-integration --output-on-failure --no-tests=error
```

CTest invokes Xvfb and gives each WebKitGTK test private XDG data/cache paths. The Ubuntu CI
lane runs these tests with timeouts. See [WebKitGTK verification notes](docs/WEBKITGTK.md)
for display and container setup.

On Windows, the same Jasmine and lifecycle suites, plus WebView2-specific cases, run in WebView2 (Evergreen runtime required):

```powershell
cmake -S . -B build-webview2 -DWEBFRONT_EMBED_WEBVIEW2=ON -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON -DWEBFRONT_DEMO_FRONTEND=WebView2 -DWEBFRONT_WEBTEST_FRONTEND=WebView2
cmake --build build-webview2 --config Release --parallel
ctest --test-dir build-webview2 -C Release --output-on-failure
```

The `windows-2025` CI matrix includes both the default MSVC build and this WebView2 configuration. Every browser test has a CTest timeout. Lifecycle tests cover repeated opening, closure requested during native window creation, queued startup timer messages after closure or failure, a held page resource after runtime creation, a page that never connects, a rejected `webFront.ready`, initial and later failed navigation, incompatible COM and a missing runtime. Unit tests cover shared URL construction, configured addresses and deadlines, process-failure policy, native focus/move/DPI forwarding, reentrant shutdown and hexadecimal HRESULT diagnostics.

The shared Jasmine suite also runs the actual Signal Studio React interface against its C++ callback, including parameter changes and recovery after a failed call. Unit tests check the generated samples and parameter validation.

CI runs the WebView2 browser label separately with `--no-tests=error`, so absent browser tests fail the job. Runtime provisioning uses Microsoft's mutable Evergreen bootstrapper redirect, verifies its Microsoft signature if downloaded, and logs installed executable versions. The runtime is intentionally not pinned; the browser suite verifies initialisation with the selected runtime.

## Signal Studio: the React example

[Signal Studio](src/react.html) is the application pictured above. Adjust the frequency, amplitude and harmonic mix, then generate a new waveform. An ordinary [C++ function](src/SignalDemo.hpp) calculates 256 samples and returns a numeric typed array; React renders the curve and derives its peak and RMS measurements. The data is synthetic, rather than a live device feed.

React and React DOM 19.3.1 share a locally embedded ES-module runtime; Babel 8.0.7 translates the editable JSX with the automatic transform at run time. The example needs no npm install, CDN or separate JavaScript build. Jasmine 7.0.2 and xterm 6.0.0 / Fit 0.11.0 are also embedded locally. JavaScript/CSS dependency assets use reproducible gzip; see [versions, regeneration and size measurements](docs/EMBEDDED_ASSETS.md).

To open it in a native window on Windows, configure from the repository root (WebView2 Evergreen Runtime required):

```powershell
cmake -S . -B build-webview2 -DWEBFRONT_EMBED_WEBVIEW2=ON -DWEBFRONT_DEMO_FRONTEND=WebView2
cmake --build build-webview2 --config Release --target WebFrontApp
./build-webview2/src/Release/WebFrontApp.exe react.html
```

This is how the screenshot was captured: the real application in its WebView2 window, including the Windows title bar and window controls. The same React page also runs in [CEF](#embedded-window-with-cef-opt-in), [WKWebView](#embedded-window-with-wkwebview-macos-opt-in) and [WebKitGTK](#embedded-window-with-webkitgtk-linux-opt-in). Enable the chosen adapter and set `WEBFRONT_DEMO_FRONTEND` to `CEF`, `WKWebView` or `WebKitGTK`. In the default build, launch `./src/WebFrontApp react.html` from the build directory to use the system browser instead.

## Native-module demo

Without a page argument, `WebFrontApp` serves `src/module-demo.html`, whose entry module imports other `.mjs` files with relative URLs, calls the registered C++ `moduleReady` callback and receives a C++ call back:

```bash
./src/WebFrontApp               # native-module demo
./src/WebFrontApp react.html    # Signal Studio (embedded React)
```

## Bridge basics

Built-in frontends open the configured server address. Page arguments are URI references: `app.show("index.html?view=details#result")` preserves the query and fragment. Spaces and UTF-8 characters are encoded automatically; use `%23`, `%3F` and `%25` for literal `#`, `?` and `%` in a filename. Existing percent escapes are preserved consistently across frontends.

- The two directions differ. JavaScript obtains a registered callback with `webFront.cppFunction('name')` and calls C++; the call always returns a Promise, even for a `void` C++ callback, so await it or handle its rejection (an ignored rejected Promise raises an unhandled rejection).
- C++ obtains a browser function with `ui.jsFunction("name")` and calls served JavaScript. Only the untyped form is fire-and-forget; `ui.jsFunction<R>(name)(...)` returns a `std::future<R>`.
- `co_await ui.jsFunctionSender<R>(name)(...)` starts a typed call when awaited and supports receiver stop-token cancellation. Cancellation stops the C++ wait; JavaScript already dispatched continues. The future API remains available. See [structured execution and cancellation](docs/ASYNC_EXECUTION.md).
- Bridge parameters include scalars, strings, tuples, numeric typed arrays and `std::bitset<N>`. C++ `std::vector`, `std::array` and `std::span` map to the matching JavaScript typed-array class; ordinary JavaScript arrays remain tuple-like argument groups. Bitsets map to `Uint8Array` with exactly `ceil(N / 8)` bytes: bit `i` is bit `i % 8` of byte `i / 8` (least significant first). Unused high bits must be zero; wrong types, lengths, truncated data and nonzero padding reject the call. `std::bitset<0>` maps to an empty byte array. JavaScript must know `N` from the application contract; it is not sent separately.
- Missing functions, callback exceptions, malformed returns and disconnected browsers complete a C++ future exceptionally (`get()` throws) and reject a JavaScript Promise.
- A callback whose first parameter is the application's `UI` type receives the browser that called it, for example to call it back later; JavaScript passes only the other parameters:
  ```cpp
  app.cppFunction("subscribe", [](webfront::UI caller, std::string topic) {
      caller.jsFunction("onEvent")(topic);
  });
  ```
- `onUIStarted(handler)` sets the application's handler for each connected browser. Code that must not replace it, such as a reusable component, uses `auto subscription = app.subscribeUIStarted(handler);`; the handler stays registered while `subscription` lives.

WebFront owns spawned coroutines, runs them on its server thread and cancels and drains them at shutdown. No execution context or scope is needed:

```cpp
#include <WebFront.hpp>

webfront::Task showSquareRoot(webfront::UI ui) {
    auto result = co_await ui.jsFunctionSender<double>("Math.sqrt")(81);
    co_await ui.jsFunctionSender("console.log")("sqrt(81) =", result);
}

int main() {
    webfront::WebFront app;
    app.onUIStarted([&app](webfront::UI ui) {
        app.spawn(showSquareRoot(ui));
    });
    app.show();
}
```

This is the compiled example [AwaitBrowser.cpp](src/AwaitBrowser.cpp); run `./src/WebFrontAwaitBrowser` from the build directory and open the browser console. `ui.spawn(task)` is also available. Both spawn methods accept calls from any thread while the application lives; coroutine bodies run on the server thread. A call rejected during shutdown returns an already stopped handle.

`app.shutdown()` waits for already admitted calls to finish starting their tasks before draining owned work.
Call it outside server callbacks, after external callers of `run()`/`runOne()` have returned.
A task that ignores cancellation and waits for external completion can still cause explicit shutdown
to report a stalled join; destruction waits for that completion.

Keep the returned handle to cancel one task with `handle.requestStop()`; discarding the handle keeps the task running. Catch browser errors inside the coroutine when recovery is needed; unhandled errors are logged. Do not block the server thread with lengthy work. See [execution and lifetime rules](docs/ASYNC_EXECUTION.md).

## Advanced: frontend aliases

```cpp
using BrowserFront = webfront::WebFrontWithFrontend<webfront::frontend::DefaultBrowserFrontend>;
using EmbeddedFront = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
using WindowsFront = webfront::WebFrontWithFrontend<webfront::frontend::WebView2Frontend>;
using MacFront = webfront::WebFrontWithFrontend<webfront::frontend::WKWebViewFrontend>;
using NativeFront = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
using LinuxFront = webfront::WebFrontWithFrontend<webfront::frontend::WebKitGTKFrontend>;
```

`NativeWebViewFrontend` is a pure alias: `WebView2Frontend` on Windows, `WKWebViewFrontend`
on macOS and `WebKitGTKFrontend` on Linux. Enable the corresponding CMake option
(`WEBFRONT_EMBED_WEBVIEW2`, `WEBFRONT_EMBED_WKWEBVIEW` or `WEBFRONT_EMBED_WEBKITGTK`).
At runtime, selecting the alias without that support throws the adapter's existing exception
naming the platform option. CMake selectors using `REQUIRE_AVAILABLE` instead reject the
configuration with `requires WEBFRONT_EMBED_<ADAPTER>=ON`; the module demo permits
configuration without support so it can report the runtime diagnostic.
The alias is declared only on Windows, macOS and Linux; CMake rejects `Native` on other platforms.
The adapters' runtime, main-thread and deployment requirements still apply. The alias never
changes `webfront::WebFront`, which always uses the system browser.

The module demo accepts `-DWEBFRONT_DEMO_FRONTEND=Native`. For example, on Linux:

```bash
cmake -S . -B build-native -DWEBFRONT_EMBED_WEBKITGTK=ON -DWEBFRONT_DEMO_FRONTEND=Native
cmake --build build-native --target WebFrontApp
./build-native/src/WebFrontApp
```

Native engines differ: Windows uses Chromium through WebView2; macOS and Linux use
WebKit. Choose [CEF](#embedded-window-with-cef-opt-in) when identical rendering across
platforms is required, and embed the same CEF version on every platform.

See [AGENTS.md](AGENTS.md) for architecture, development commands, conventions, test requirements and safety guidance. The current evidence-based status and backlog recommendations are in [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) and [docs/ISSUE_AUDIT.md](docs/ISSUE_AUDIT.md).

The [architecture decision records](docs/adr/README.md) explain the design choices, their Git history, tradeoffs and implementation rules for future changes and code generation.

Optional mddlog diagnostics are available through `WEBFRONT_USE_MDDLOG=ON` (default: `OFF`). Consumers keep including `tooling/Logger.hpp`; WebFront links its compiled adapter. See [dependencies, toolchain matrix and verification](docs/mddlog-integration.md).
