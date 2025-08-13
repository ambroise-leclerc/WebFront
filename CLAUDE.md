# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WebFront is a C++23 header-only library that provides rich cross-platform UI (TypeScript-based) to C++ applications with a non-invasive API. It implements WebSocket protocol over an embedded web server and enables bidirectional function calls between C++ and JavaScript with native type conversions.

## Build System and Commands

### Build Configuration
```bash
# Configure build (Release by default)
cmake . -B build

# Configure with Debug and coverage
cmake . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON

# Build the project
cmake --build build --config Release --parallel
```

### Testing
```bash
# Run all tests
cd build && ctest --verbose -C Release

# Rerun failed tests with detailed output
cd build && ctest --rerun-failed --output-on-failure -C Release
```

### Development Options
- `ENABLE_TESTING=ON` (default): Build test executables
- `ENABLE_COVERAGE=ON`: Enable test coverage collection (requires Debug build)
- `BUILD_SHARED_LIBS=OFF` (default): Build as static library

## Architecture

### Core Components

**WebFront Main Class** (`include/WebFront.hpp`):
- Template-based design with pluggable networking and filesystem layers
- `BasicWF<NetProvider, Filesystem>` is the main class template
- Default uses `TCPNetworkingTS` for networking
- Provides bidirectional C++/JS function calling via WebLink protocol

**HTTP Server** (`include/http/HTTPServer.hpp`):
- Minimal HTTP/1.1 implementation with WebSocket upgrade support
- Serves static files and handles WebSocket connections for UI communication
- Built-in MIME type detection and content encoding support

**File System Abstraction** (`include/system/`):
- Multiple filesystem implementations: NativeFS, IndexFS, ReactFS, BabelFS, JasmineFS
- Support for virtual filesystems and in-memory static data
- Template-based design with encoding support (gzip, etc.)

**WebLink Protocol** (`include/weblink/`):
- Custom protocol for C++/JS communication over WebSocket
- Handles function calls, message passing, and UI lifecycle events
- Type-safe serialization for native C++ types

**Frontend Backends** (`include/frontend/`):
- CEF (Chromium Embedded Framework) support for native windows
- DefaultBrowser fallback for system browser integration
- Conditional compilation based on available dependencies

### Networking Layer
Uses networking-ts-impl library for cross-platform networking with fallbacks:
- Primary: C++ Networking TS implementation
- Platform-specific socket implementations when TS unavailable

### Testing Framework
- Uses Catch2 v3.4.0 for unit testing
- Comprehensive test coverage for all major components
- Separate webtest directory for JavaScript/UI integration tests

## CEF Integration Notes

The project has complex CEF integration with platform-specific handling:
- Windows: Manual Debug/Release library linking and DLL copying
- macOS: Framework-based integration with compatibility workarounds
- Linux: Shared library integration
- CEF tests and wrapper builds are explicitly disabled to avoid binary distribution issues

## Key Development Patterns

### Template Design
- Heavy use of C++23 concepts for type constraints
- Template specialization for different networking and filesystem providers
- CRTP patterns for extensible UI components

### Cross-Platform Compatibility
- Extensive platform-specific preprocessor directives
- Windows-specific WinSock initialization and macro prevention
- Apple-specific compiler feature definitions for std::format

### Function Registration
```cpp
// Register C++ function callable from JS
webFront.cppFunction<ReturnType, Args...>("functionName", [](Args... args) {
    // Implementation
});

// Call JS function from C++
auto jsFunc = ui.jsFunction("jsFunctionName");
jsFunc(args...);
```