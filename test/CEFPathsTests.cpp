#include <catch2/catch_test_macros.hpp>
#include <frontend/CEFPaths.hpp>

#include <filesystem>

SCENARIO("macOS CEF paths match the deployed framework layout") {
    GIVEN("executables in different CMake target directories") {
        const std::filesystem::path buildRoot{"build-cef"};
        const auto                  deployedFramework = buildRoot / "Frameworks" / "Chromium Embedded Framework.framework";

        WHEN("each target resolves its framework path") {
            THEN("both locate the framework deployed in the parent directory") {
                REQUIRE(webfront::cef::detail::macOSFrameworkPath(buildRoot / "src" / "WebFrontApp") == deployedFramework);
                REQUIRE(webfront::cef::detail::macOSFrameworkPath(buildRoot / "webtest" / "webtest") == deployedFramework);
            }
        }
    }

    GIVEN("an application bundle with an arbitrary executable name") {
        const std::filesystem::path contents = std::filesystem::path{"CustomApp.app"} / "Contents";

        WHEN("the executable resolves its framework path") {
            THEN("it uses Contents/Frameworks, matching the CEF library loader") {
                REQUIRE(webfront::cef::detail::macOSFrameworkPath(contents / "MacOS" / "CustomApp")
                        == contents / "Frameworks" / "Chromium Embedded Framework.framework");
            }
        }
    }
}
