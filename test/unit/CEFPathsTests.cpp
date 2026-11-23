#include <webfront/frontend/CEFPaths.hpp>
#include <webfront/testing/Bdd.hpp>

#include <filesystem>

using namespace webfront::bdd;

const Scenario macOSCEF{"macOS CEF paths match the deployed framework layout", [] {
                            given("executables in different CMake target directories", [&] {
                                const std::filesystem::path buildRoot{"build-cef"};
                                const auto                  deployedFramework = buildRoot / "Frameworks" / "Chromium Embedded Framework.framework";

                                when("each target resolves its framework path", [&] {
                                    then("both locate the framework deployed in the parent directory", [&] {
                                        require(webfront::cef::detail::macOSFrameworkPath(buildRoot / "examples" / "HelloWorld") == deployedFramework);
                                        require(webfront::cef::detail::macOSFrameworkPath(buildRoot / "webtest" / "webtest") == deployedFramework);
                                    });
                                });
                            });

                            given("an application bundle with an arbitrary executable name", [&] {
                                const std::filesystem::path contents = std::filesystem::path{"CustomApp.app"} / "Contents";

                                when("the executable resolves its framework path", [&] {
                                    then("it uses Contents/Frameworks, matching the CEF library loader", [&] {
                                        require(webfront::cef::detail::macOSFrameworkPath(contents / "MacOS" / "CustomApp")
                                                == contents / "Frameworks" / "Chromium Embedded Framework.framework");
                                    });
                                });
                            });
                        }};
