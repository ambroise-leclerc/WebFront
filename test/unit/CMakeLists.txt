CPMAddPackage("gh:catchorg/Catch2@3.15.2")
target_compile_features(Catch2 PRIVATE cxx_std_23)
target_compile_features(Catch2WithMain PRIVATE cxx_std_23)
# Keep inline standard-library definitions consistent across the test executable.
# Instrument the test runtime, but leave dependency sources out of project coverage.
add_library(WebFront_test_sanitizers INTERFACE)
enable_sanitizers(WebFront_test_sanitizers NO_COVERAGE)
target_link_libraries(Catch2 PRIVATE WebFront_test_hardening WebFront_test_sanitizers)
target_link_libraries(Catch2WithMain PRIVATE WebFront_test_hardening WebFront_test_sanitizers)

find_package(Threads)
include(CTest)

set(TESTS_LIST)
list(APPEND TESTS_LIST WebKitGTKPolicyTests.cpp ContentPolicyTests.cpp HTTPServerTests.cpp HTTPEncodingTests.cpp AcceptEncodingTests.cpp GzipTests.cpp ModuleHTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST FileSystemTests.cpp NativeFSTests.cpp)
list(APPEND TESTS_LIST DefaultFSTests.cpp StdnetShutdownTests.cpp SafePathTests.cpp UISubscribersTests.cpp)
list(APPEND TESTS_LIST WebFrontTests.cpp WebLinkTests.cpp CEFPathsTests.cpp CEFMacrosTests.cpp)
list(APPEND TESTS_LIST DeveloperTests.cpp LifeTests.cpp StructuredShutdownTests.cpp JsFunctionSenderTests.cpp ApplicationTaskTests.cpp)
list(APPEND TESTS_LIST SignalDemoTests.cpp FrontendUrlTests.cpp ApplicationUrlTests.cpp FrontendContractTests.cpp CommandsTests.cpp)
if(WEBFRONT_USE_MDDLOG)
  list(APPEND TESTS_LIST MddlogLoggerTests.cpp MddlogTransportTests.cpp)
endif()
if(WEBFRONT_EMBED_WEBVIEW2)
  list(APPEND TESTS_LIST WebView2Tests.cpp)
endif()
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE WebFront_test_hardening WebFront_warnings WebFront_options Catch2::Catch2WithMain WebFront)
configure_file(Version.hpp.in generated/Version.hpp @ONLY)
target_include_directories(tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(tests PROPERTIES TIMEOUT 60)

add_test(NAME ReleaseVersionConsistency
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseVersion.cmake)
add_test(NAME ReleaseVersionRegression
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/release-version-regression
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseVersionTests.cmake)
add_test(NAME BootstrapProvisioning
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-provisioning
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckBootstrapProvisioning.cmake)

# Windows bash.exe may be the WSL launcher rather than a native Bash runtime.
if(UNIX)
    find_program(WEBFRONT_TEST_BASH_EXECUTABLE bash)
    find_package(Git QUIET)
    if(WEBFRONT_TEST_BASH_EXECUTABLE AND Git_FOUND)
        add_test(NAME NoAiAttributionRegression
            COMMAND "${WEBFRONT_TEST_BASH_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/.github/scripts/test-no-ai-attribution.sh")
        # The many short Git/grep subprocesses exceeded 60 seconds on hosted macOS
        # with four CTest workers. Allow contention while keeping the test bounded.
        set_tests_properties(NoAiAttributionRegression PROPERTIES TIMEOUT 180)
    endif()
endif()

if(ENABLE_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()

if(WEBFRONT_USE_MDDLOG)
  # This consumer includes only the public header and links only the facade target.
  add_executable(mddlog_header_consumer MddlogHeaderConsumer.cpp)
  target_link_libraries(mddlog_header_consumer PRIVATE WebFront::mddlog)
  set_target_properties(mddlog_header_consumer PROPERTIES CXX_SCAN_FOR_MODULES OFF CXX_MODULE_STD OFF)
  add_test(NAME MddlogHeaderConsumer COMMAND mddlog_header_consumer)
endif()

# The documented newcomer commands and snippets must keep matching the built examples and CI.
add_test(NAME RepositoryLayout
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckRepositoryLayout.cmake)
add_test(NAME NewcomerDocsMatchExamples
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckNewcomerDocs.cmake)
add_test(NAME FrontendSelection
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckFrontendSelection.cmake)
# Exercise configuration-dependent CTest discovery and generator switches on single-config CI hosts.
# The fixture configures no compiler and builds nothing; Ninja is optional for contributor builds.
find_program(WEBFRONT_TEST_NINJA_EXECUTABLE ninja)
if(WEBFRONT_TEST_NINJA_EXECUTABLE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frontend-multiconfig")
    add_test(NAME FrontendSelectionMultiConfig
        COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckFrontendGenerators.cmake)
    set_tests_properties(FrontendSelectionMultiConfig PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frontend-multiconfig")
else()
    message(STATUS "Skipping FrontendSelectionMultiConfig: Ninja was not found; FrontendSelection still runs")
endif()
# The project bootstrap exports CPM_DOWNLOAD_LOCATION. If CPM provisioning changes, pass the
# equivalent module path here so the offline fixture still exercises the same CPM as this build.
add_test(NAME CEFTestingState
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DCPM_MODULE=${CPM_DOWNLOAD_LOCATION}
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckCEFTestingState.cmake)
# The Linux CEF file copy, with stand-in files: destinations, incremental copies and refusals.
add_test(NAME CEFCopyDestinations
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckCEFCopy.cmake)
add_test(NAME WebView2UnsupportedToolchain
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckWebView2Toolchain.cmake)
add_test(NAME WebView2LoaderArchitecture
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckWebView2Architecture.cmake)
add_test(NAME WKWebViewConfiguration
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckWKWebViewSupport.cmake)

# A minimal parent project must be able to add_subdirectory(WebFront) and link WebFront::WebFront
# without building WebFront's examples or tests. Dependencies reuse this build's sources, so the
# test needs no network access.
include(FetchContent)
FetchContent_GetProperties(stdexec)
set(_consumer_dependency_args -DCPM_stdnet_SOURCE=${stdnet_SOURCE_DIR})
if(stdexec_POPULATED)
    list(APPEND _consumer_dependency_args -DFETCHCONTENT_SOURCE_DIR_STDEXEC=${stdexec_SOURCE_DIR})
endif()
# This is a separate CMake configure: preserve the caller's explicit module-scanning policy.
# In particular, Ninja/GCC CI disables scanning to avoid GCC's stdexec -fmodules-ts crash.
if(DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
    list(APPEND _consumer_dependency_args -DCMAKE_CXX_SCAN_FOR_MODULES:BOOL=${CMAKE_CXX_SCAN_FOR_MODULES})
endif()
# Read the actual target's launcher at generation time, including an explicitly
# configured or auto-discovered cache. Do not depend on Cache.cmake's local variable.
add_test(NAME AddSubdirectoryConsumer
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${PROJECT_SOURCE_DIR}/test/cmake/consumer ${CMAKE_CURRENT_BINARY_DIR}/consumer-build
        --build-generator ${CMAKE_GENERATOR}
        --build-config $<CONFIG>
        --build-options -DWEBFRONT_SOURCE_DIR=${PROJECT_SOURCE_DIR} ${_consumer_dependency_args}
            -DENABLE_CACHE=OFF "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=$<TARGET_PROPERTY:tests,CXX_COMPILER_LAUNCHER>"
            -DCMAKE_BUILD_TYPE=$<CONFIG> -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
set_tests_properties(AddSubdirectoryConsumer PROPERTIES TIMEOUT 600)
