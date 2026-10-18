# Changelog

## Unreleased

### Changed

- JavaScript bridge arguments or results outside the requested non-boolean C++ integer range, including NaN and infinities, now fail decoding instead of triggering undefined conversion behavior. A JavaScript call rejects its Promise; a typed C++ call completes its future exceptionally.
- Tests enable vendor standard-library hardening and, when supported, pattern initialization. Optional sanitizer builds and bounded decoder fuzzing remain private to internal targets.
- Demo and browser-test frontend selection, SDK availability and test wiring share a CMake registry; existing selector names and defaults are preserved.
- `WEBFRONT_EMBED_CEF=ON` now fails during configuration outside Windows, Linux and macOS (`Darwin`), before downloading dependencies. This includes FreeBSD configurations that previously reached dependency setup.
- CEF system include directories derived from `CEF_ROOT` are added only when `WEBFRONT_EMBED_CEF` is enabled, rather than whenever the directory exists.
- CEF's `BUILD_TESTING=OFF` setting is scoped to its dependency through CPM. Other inherited CEF configuration flags forced into the cache remain unchanged.
- `BUILD_TESTING` is no longer overwritten globally, including in CEF-off builds. When testing is enabled, CTest can now use its normal `ON` default and generate dashboard targets (`Experimental`, `Nightly`, `Continuous`) and `DartConfiguration.tcl`; callers can set `BUILD_TESTING` explicitly.
- CEF setup no longer writes late macOS patches or an empty wrapper stub into dependency sources. Upstream demo targets are excluded from the default build, and a missing or non-static `libcef_dll_wrapper` fails during configuration.
- Browser selectors retain their cached choice on reconfiguration. CEF runners on Windows/macOS now warn that no browser test is registered; set `WEBFRONT_WEBTEST_FRONTEND=WebView2` explicitly to switch a Windows runner.

### Fixed

- HTTP requests retain their own partial method name when receives from multiple connections interleave.
- Input decoding no longer indexes past empty wire strings or unknown HTTP method names, and rejects overflowing HTTP version fields.
