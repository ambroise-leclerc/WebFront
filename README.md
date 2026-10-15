# WebFront

**A browser UI for your C++ application in a few lines of C++** — no Node.js, no bundler and no external web server needed for Hello World. WebFront is a C++23 header-only library that embeds an HTTP/WebSocket server, serves a ready-made page, and connects that page to your C++ code.

> **Status: experimental.** WebFront is a foundation, not a production-ready UI framework. See [Limitations](#limitations).

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
mkdir build
cd build
cmake ..
cmake --build .
```

Then launch it from the build directory:

```bash
./src/HelloWorld
```

Your system browser opens a "Hello, WebFront!" page. WebFront serves it from an embedded fallback, so you did not write any HTML or JavaScript. The status badge reads **Connected to WebFront** only after the browser-to-C++ WebSocket handshake completed; if the connection fails, the page shows an error instead. Press Enter in the terminal to stop the server.

![The embedded Hello, WebFront! page showing the connected indicator](docs/images/hello-world.png)

## Call C++ from a page: the greeter

`WebFrontGreeter` adds a small editable UI that calls real C++ logic. The C++ side registers a callback; its signature is deduced from the lambda:

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

## A terminal for your application

`WebFrontTerminal` gives a C++ program a command-line interface in the browser: an embedded [xterm.js](https://xtermjs.org/) terminal whose line editor (history, Tab completion) sends each line to commands registered in C++. It is the first opt-in UI component; it needs no document root and no network access.

```cpp
#include <components/Terminal.hpp>

webfront::TerminalApp app;
webfront::Terminal terminal{app, {.prompt = "demo> "}};
terminal.command("greet", "Greet someone", [](webfront::CommandArgs args) {
    return "Hello, " + (args.empty() ? std::string{"world"} : args[0]) + "!";
});
app.show(terminal.page);
```

A command's return value (or what it writes to a `std::ostream&` parameter) is printed; an exception is shown as an error. `help` and `clear` are built in. Output appears when the command finishes: there is no streaming or interruption yet. See [docs/UI_COMPONENTS.md](docs/UI_COMPONENTS.md) for the component architecture and the `wf` tool manager.

```bash
./src/WebFrontTerminal
```

`WebFrontTools` (`src/Tools.cpp`) adds the `wf` tool manager to the terminal: `wf search`, `wf install <package>[@version]`, `wf list`, `wf open` and `wf remove` manage web tools published on npm with the `webfront-tool` keyword. The page downloads them from registry.npmjs.org, checks their SHA-512 integrity and stores them in the working directory (`.webfront/tools/`, recorded in `webfront-tools.lock.json`). An installed tool can call every registered C++ function: install only tools you trust, during development.

## Embedded window with CEF (opt-in)

By default WebFront opens your system browser. To get an application window instead, enable the Chromium Embedded Framework when configuring. CEF is a **large download** and is never fetched unless you ask for it:

```bash
cmake -DWEBFRONT_EMBED_CEF=ON ..
```

`WEBFRONT_EMBED_CEF` only makes CEF *available*; the frontend is chosen in your source by changing the alias, nothing else:

```cpp
using App = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
```

Selecting `CEFFrontend` in a build without CEF fails with a clear error. An embedded window stops the server when it closes. `webfront::WebFront` always uses the system browser, even in a CEF-enabled build.

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
- Calls are asynchronous. Results cross the bridge as `std::future` (C++ calling JavaScript, `ui.jsFunction<R>(name)`) and Promises (JavaScript calling C++, `webFront.cppFunction(name)`), limited to scalars, strings (up to 65535 bytes each), tuples and numeric typed arrays. Richer objects and a DOM abstraction are out of scope.
- A configured document root exposes local files for **development only**; it is not a sandbox or a production packaging format.
- Nothing authenticates the browser: the server binds to loopback by default, but any local process can connect.
- The tested toolchains are listed in the CI workflow; other compilers may not work yet.

## Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `WEBFRONT_EMBED_CEF` | `OFF` | Download and link CEF so `CEFFrontend` is available |
| `ENABLE_TESTING` | `OFF` | Build the Catch2 suite and the Jasmine browser test |
| `WEBFRONT_BUILD_EXAMPLES` | `ON` when top-level | Build `HelloWorld`, `WebFrontGreeter` and `WebFrontApp` |
| `ENABLE_COVERAGE` | `OFF` | Instrument the C++ tests (GCC/Clang) |

## Contributing: build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The automated real-browser test runs on Linux with Xvfb:

```bash
cmake -S . -B build-cef -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=ON -DENABLE_TESTING=ON
cmake --build build-cef --target webtest --parallel
ctest --test-dir build-cef -L web-integration --output-on-failure
```

The Jasmine specs are native ES modules. They prove relative module loading, both bridge directions, all supported numeric typed arrays and `webFront.ready`; report the final result to C++; close the CEF window automatically; and fail CTest on an assertion, bridge, startup or timeout error. CTest also checks that the commands and snippets in this README match the built examples.

## Native-module demo and React example

`WebFrontApp` serves `src/module-demo.html`, whose entry module imports other `.mjs` files with relative URLs, calls the registered C++ `moduleReady` callback and receives a C++ call back. The older React/Babel example remains available explicitly:

```bash
./src/WebFrontApp               # native-module demo
./src/WebFrontApp react.html    # React/Babel example
```

## Bridge basics

- The two directions differ. JavaScript obtains a registered callback with `webFront.cppFunction('name')` and calls C++; the call always returns a Promise, even for a `void` C++ callback, so await it or handle its rejection (an ignored rejected Promise raises an unhandled rejection).
- C++ obtains a browser function with `ui.jsFunction("name")` and calls served JavaScript. Only the untyped form is fire-and-forget; `ui.jsFunction<R>(name)(...)` returns a `std::future<R>`.
- Bridge parameters include scalars, strings, tuples and numeric typed arrays. C++ `std::vector`, `std::array` and `std::span` map to the matching JavaScript typed-array class; ordinary JavaScript arrays remain tuple-like argument groups.
- Missing functions, callback exceptions, malformed returns and disconnected browsers complete a C++ future exceptionally (`get()` throws) and reject a JavaScript Promise.
- A callback whose first parameter is the application's `UI` type receives the browser that called it, for example to call it back later; JavaScript passes only the other parameters:
  ```cpp
  app.cppFunction("subscribe", [](webfront::UI caller, std::string topic) {
      caller.jsFunction("onEvent")(topic);
  });
  ```
- `onUIStarted(handler)` sets the application's handler for each connected browser. Code that must not replace it, such as a reusable component, uses `auto subscription = app.subscribeUIStarted(handler);`; the handler stays registered while `subscription` lives.

## Advanced: frontend aliases

```cpp
using BrowserFront = webfront::WebFrontWithFrontend<webfront::frontend::DefaultBrowserFrontend>;
using EmbeddedFront = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
```

See [AGENTS.md](AGENTS.md) for architecture, development commands, conventions, test requirements and safety guidance. The current evidence-based status and backlog recommendations are in [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) and [docs/ISSUE_AUDIT.md](docs/ISSUE_AUDIT.md).

Optional mddlog diagnostics are available through `WEBFRONT_USE_MDDLOG=ON` (default: `OFF`). Consumers keep including `tooling/Logger.hpp`; WebFront links its compiled adapter. See [dependencies, toolchain matrix and verification](docs/mddlog-integration.md).
