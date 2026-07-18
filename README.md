# WebFront

WebFront is an experimental C++23 header-only library for browser-based user interfaces. It serves local HTML/JavaScript through an embedded HTTP server and connects the page to C++ over WebSocket.

The current baseline demonstrates asynchronous, fire-and-forget calls in both directions:

- JavaScript obtains a registered callback with `webFront.cppFunction('name')` and calls C++.
- C++ obtains a browser function with `ui.jsFunction("name")` and calls served JavaScript.

Function return values and remote exception propagation are not implemented yet.

## Build and test

The normal build avoids the large optional CEF dependency and runs the portable Catch2 suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Relevant options are:

- `WEBFRONT_EMBED_CEF=OFF`: open examples in the system browser.
- `WEBFRONT_EMBED_CEF=ON`: build the embedded Chromium frontend and browser integration test.
- `ENABLE_TESTING=ON`: build Catch2 and Jasmine test targets.
- `ENABLE_COVERAGE=ON`: instrument the C++ tests with GCC/Clang coverage flags.

CPM downloads Networking TS during configuration. It also downloads Catch2 when `ENABLE_TESTING=ON` and CEF when
`WEBFRONT_EMBED_CEF=ON`.

## Minimal native-module demo

Run the default example after a CEF-off build:

```bash
./build/src/WebFrontApp
```

The application serves `src/module-demo.html`, whose entry module imports the other `.mjs` files with relative URLs. After the WebSocket is ready, the module calls the registered C++ `moduleReady` callback; C++ then calls `webfrontModule.receiveFromCpp`, a function installed by the served module. The page button demonstrates the reverse JavaScript-to-C++ call.

The older React/Babel example remains available explicitly:

```bash
./build/src/WebFrontApp react.html
```

When the system-browser frontend is used, press Enter in the application terminal after closing the page. A CEF-enabled build uses an embedded window and stops when that window closes.

## Automated browser integration

On Linux with Xvfb available:

```bash
cmake -S . -B build-cef -DCMAKE_BUILD_TYPE=Release \
  -DWEBFRONT_EMBED_CEF=ON -DENABLE_TESTING=ON
cmake --build build-cef --target webtest --parallel
ctest --test-dir build-cef -L web-integration --output-on-failure
```

The Jasmine specs are native ES modules. They prove relative module loading and both bridge directions, report the final result to C++, close the CEF window automatically, and fail CTest on an assertion, bridge, startup, or timeout error.

## Selecting a frontend

`webfront::WebFront` uses the frontend selected by the build. Applications can select one explicitly:

```cpp
using BrowserFront = webfront::WebFrontWithFrontend<webfront::frontend::DefaultBrowserFrontend>;
using EmbeddedFront = webfront::WebFrontWithFrontend<webfront::frontend::CEFFrontend>;
```

See [AGENTS.md](AGENTS.md) for architecture, development commands, conventions, test requirements, and safety guidance. The current evidence-based status and backlog recommendations are in [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) and [docs/ISSUE_AUDIT.md](docs/ISSUE_AUDIT.md).
