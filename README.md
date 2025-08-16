# WebFront

A C++23 header-only library with the aim of providing rich cross-platform UI (Typescript based) to C++ applications with a non-invasive API.

WebFront implements the websocket protocol over an embedded Web server and provides CppToJs and JsToCpp cross functions calls with native types conversions.

[![Windows MSVC](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/ambroise-leclerc/317c22bfe80b2b51663187fbebfba533/raw/windows-latest-msvc.json)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Ubuntu GCC](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/ambroise-leclerc/317c22bfe80b2b51663187fbebfba533/raw/ubuntu-latest-gcc.json)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![Ubuntu Clang](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/ambroise-leclerc/317c22bfe80b2b51663187fbebfba533/raw/ubuntu-latest-clang.json)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![MacOS Clang](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/ambroise-leclerc/317c22bfe80b2b51663187fbebfba533/raw/macos-latest-clang.json)](https://github.com/ambroise-leclerc/WebFront/actions/workflows/BuildAndTest.yml)
[![codecov](https://codecov.io/github/ambroise-leclerc/WebFront/branch/master/graph/badge.svg?token=ODE6O36XIV)](https://codecov.io/github/ambroise-leclerc/WebFront)

[![CodeScene Code Health](https://codescene.io/projects/29377/status-badges/code-health)](https://codescene.io/projects/29377)
[![CodeScene System Mastery](https://codescene.io/projects/29377/status-badges/system-mastery)](https://codescene.io/projects/29377)

## Getting started

#### Hello World
```cpp
    WebFront webfront;
    webFront.cppFunction<void, std::string>("print", [](std::string text) {
        std::cout << text << '\n';
    });

    webFront.onUIStarted([](UI ui) {
        ui.addScript("var addText = function(text, num) {                 "
                     "  let print = webFront.cppFunction('print');        "
                     "  print(text + ' of ' + num);                       "
                     "}                                                   ");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2023);
    });

```

## Building and Testing

### Build Configuration
```bash
# Configure build (Release by default with embedded CEF)
cmake . -B build

# Configure with Debug and coverage  
cmake . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON

# Configure without embedded CEF (uses system browser)
cmake . -B build -DWEBFRONT_EMBED_CEF=OFF

# Build the project
cmake --build build --config Release --parallel
```

#### CMake Options
- `WEBFRONT_EMBED_CEF=ON` (default): Enable embedded CEF window support for chromeless application windows
- `WEBFRONT_EMBED_CEF=OFF`: Disable CEF, applications will use the system browser instead
- `ENABLE_TESTING=ON` (default): Build unit tests
- `ENABLE_COVERAGE=ON`: Enable test coverage collection

### Running Examples

#### WebFrontApp - React Example with Embedded Window
The main example application demonstrates React integration with TypeScript/Babel transpilation in a chromeless CEF window:

```bash
cd build
./src/WebFrontApp
```

When built with `WEBFRONT_EMBED_CEF=ON` (default), this opens a clean chromeless application window. When built with `WEBFRONT_EMBED_CEF=OFF`, it opens in your system browser. The application provides:
- ✅ **Unified API**: `webfront::open()` automatically uses the best available window type
- ✅ **Chromeless UI**: Clean application window without browser chrome elements (CEF only)
- ✅ **React Support**: Full React 18 + JSX + Babel transpilation
- ✅ **C++/JS Interop**: Bidirectional function calls between C++ and JavaScript
- ✅ **TypeScript Ready**: Modern JavaScript features and type support

#### webtest - Jasmine Test Runner with Embedded Window  
Run the JavaScript test suite in an embedded CEF window for automated testing:

```bash
cd build
./webtest/webtest
```

This launches the Jasmine test runner using the same unified `webfront::open()` API, providing:
- ✅ **Automated Testing**: JavaScript unit tests with visual feedback
- ✅ **Embedded Test Runner**: No external browser dependencies when CEF enabled
- ✅ **CI/CD Ready**: Suitable for headless testing environments
- ✅ **WebFront API Testing**: Tests the C++/JavaScript bridge functionality

Both applications automatically use the system browser when built with `WEBFRONT_EMBED_CEF=OFF`.

### Testing
```bash
# Run all C++ unit tests
cd build && ctest --verbose -C Release

# Run JavaScript tests with embedded window
cd build && ./webtest/webtest

# Rerun failed tests with detailed output
cd build && ctest --rerun-failed --output-on-failure -C Release
```
