# The scenarios use WebFront::Bdd (include/webfront/testing/Bdd.hpp, docs/BDD.md), a test framework in a single header;
# webfront_bdd_discover_tests registers them in CTest.

find_package(Threads)
include(CTest)

# WEBFRONT_TEST_TIMEOUT_FACTOR (Bdd.hpp, a documented option) lengthens the waits of a slow job. It is read here, when CMake configures, and
# handed to every unit test as its environment, so that the waits and the CTest limits agree whatever the environment of
# ctest is (TimeoutFactor.cmake gives the rule, which the TimeoutFactor case checks).
include(TimeoutFactor.cmake)
webfront_unit_timeouts("$ENV{WEBFRONT_TEST_TIMEOUT_FACTOR}" _unit_timeout _shard_timeout _unit_test_environment)
add_test(NAME TimeoutFactor COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckTimeoutFactor.cmake)
set_tests_properties(TimeoutFactor PROPERTIES TIMEOUT 30 LABELS fast)

add_test(NAME TestSuiteRepetition COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/suite-repetition -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckTestSuiteRepetition.cmake)
set_tests_properties(TestSuiteRepetition PROPERTIES TIMEOUT 180)

# Optional maintainer tooling: the default C++ build must not require Doxygen.
find_program(WEBFRONT_TEST_DOXYGEN doxygen)
if(WEBFRONT_TEST_DOXYGEN)
  add_test(NAME DoxygenDocumentation COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/doxygen-test -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckDoxygen.cmake)
  set_tests_properties(DoxygenDocumentation PROPERTIES TIMEOUT 60)
endif()

# Optional Memcheck regression for compiler-introduced secret-dependent curve operations.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  find_program(WEBFRONT_TEST_VALGRIND valgrind)
  find_path(WEBFRONT_TEST_VALGRIND_INCLUDE valgrind/memcheck.h)
  if(WEBFRONT_TEST_VALGRIND AND WEBFRONT_TEST_VALGRIND_INCLUDE)
    add_executable(tls_constant_time TlsConstantTime.cpp)
    target_link_libraries(tls_constant_time PRIVATE WebFront WebFront_warnings)
    target_include_directories(tls_constant_time SYSTEM PRIVATE "${WEBFRONT_TEST_VALGRIND_INCLUDE}")
    target_compile_options(tls_constant_time PRIVATE -Os)
    add_test(NAME TLSConstantTime COMMAND "${WEBFRONT_TEST_VALGRIND}" --tool=memcheck --error-exitcode=1 $<TARGET_FILE:tls_constant_time>)
    set_tests_properties(TLSConstantTime PROPERTIES TIMEOUT 30)
  endif()
endif()

# The framework's own test: the order of the sections, the failures, the filters and the exit codes.
add_executable(bdd_self_test BddSelfTest.cpp)
target_link_libraries(bdd_self_test PRIVATE WebFront::Bdd WebFront_test_hardening WebFront_warnings WebFront_options)
target_compile_features(bdd_self_test PRIVATE cxx_std_23)
add_test(NAME BddSelfTest COMMAND bdd_self_test)
# CTest registers one case per scenario whatever brackets or semicolons its title holds.
add_executable(bdd_discovery_probe Main.cpp BddDiscoveryProbe.cpp)
target_link_libraries(bdd_discovery_probe PRIVATE WebFront::Bdd WebFront_test_hardening WebFront_warnings WebFront_options)
target_compile_features(bdd_discovery_probe PRIVATE cxx_std_23)
add_test(NAME BddDiscovery COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:bdd_discovery_probe>
    -DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/WebFrontBdd.cmake -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bdd-discovery
    "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckBddDiscovery.cmake)
set_tests_properties(BddSelfTest BddDiscovery PROPERTIES TIMEOUT 30 LABELS fast)

set(TESTS_LIST)
list(APPEND TESTS_LIST TlsTests.cpp)
list(APPEND TESTS_LIST WebKitGTKPolicyTests.cpp ContentPolicyTests.cpp HTTPServerTests.cpp HTTPEncodingTests.cpp GzipExecutorTests.cpp AcceptEncodingTests.cpp GzipTests.cpp ModuleHTTPServerTests.cpp HTTPRoutesTests.cpp HTTPRangeTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST FormsTests.cpp ReactStoreTests.cpp DomDocumentTests.cpp DomBatchTests.cpp JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST FileSystemTests.cpp NativeFSTests.cpp DownloadsTests.cpp StorageTests.cpp NativeStorageTests.cpp PtyTests.cpp)
list(APPEND TESTS_LIST DefaultFSTests.cpp StdnetShutdownTests.cpp ProfileLeaseTests.cpp SafePathTests.cpp UISubscribersTests.cpp)
list(APPEND TESTS_LIST ReadBindingTests.cpp ViewAssociationTests.cpp BroadcastTests.cpp WebFrontTests.cpp WebLinkTests.cpp CEFPathsTests.cpp CEFMacrosTests.cpp)
list(APPEND TESTS_LIST DeveloperTests.cpp WfDriverTests.cpp AutomationTests.cpp AutomationChannelTests.cpp AutomationDiscoveryTests.cpp AutomationMcpHttpTests.cpp JasmineConsumerTests.cpp HostTests.cpp LifeTests.cpp HostLoopTests.cpp StructuredShutdownTests.cpp JsFunctionSenderTests.cpp ApplicationTaskTests.cpp DeferredCallTests.cpp)
list(APPEND TESTS_LIST Main.cpp ScenarioIsolation.cpp FrontendUrlTests.cpp ApplicationUrlTests.cpp FrontendContractTests.cpp UIExecutorTests.cpp NativeWindowTests.cpp CommandsTests.cpp EmbeddedCoreAssetsTests.cpp)
if(WEBFRONT_USE_MDDLOG)
  list(APPEND TESTS_LIST MddlogLoggerTests.cpp MddlogTransportTests.cpp)
endif()
if(WEBFRONT_EMBED_WEBVIEW2)
  list(APPEND TESTS_LIST WebView2Tests.cpp)
endif()
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE WebFront::Bdd WebFront_test_hardening WebFront_warnings WebFront_options WebFront)
# The scene of the host-loop example: the CPU renderer everywhere, compared with the CUDA one where a toolkit was found.
target_link_libraries(tests PRIVATE WebFront_boing)
if(WIN32)
  target_link_libraries(tests PRIVATE ws2_32 bcrypt advapi32)
endif()
if(MSVC)
  # The templates of the DOM, forms and bridge tests exceed the section limit of one MSVC object file.
  target_compile_options(tests PRIVATE /bigobj)
endif()
target_compile_definitions(tests PRIVATE WEBFRONT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# The native program of the webFrontHost and runProcess tests (also run by the browser specs).
add_executable(host_probe ../web/host/HostProbe.cpp)
target_compile_features(host_probe PRIVATE cxx_std_23)
target_link_libraries(host_probe PRIVATE WebFront_test_hardening WebFront_warnings)
add_dependencies(tests host_probe)
target_compile_definitions(tests PRIVATE WEBFRONT_HOST_PROBE="$<TARGET_FILE:host_probe>")

add_executable(pty_probe ../fixtures/pty/Probe.cpp)
target_link_libraries(pty_probe PRIVATE WebFront WebFront_test_hardening WebFront_warnings)
add_dependencies(tests pty_probe)
target_compile_definitions(tests PRIVATE WEBFRONT_PTY_PROBE="$<TARGET_FILE:pty_probe>")

# The heavy units instantiate a full application, and compile four to a file in about half the time of four apart
# (docs/BUILD_TIME_STUDY.md): that is the default. 0 keeps one object per test file, which the portability and
# sanitizer jobs ask for. The light units stay apart: grouping every unit does not compile, as their
# helpers share names.
if(WEBFRONT_TEST_UNITY_BATCH GREATER 0)
  set(_heavy WebFrontTests ReadBindingTests ViewAssociationTests BroadcastTests CommandsTests ApplicationTaskTests DeferredCallTests StdnetShutdownTests FrontendContractTests JsFunctionSenderTests
      AutomationTests ReactStoreTests DeveloperTests CEFMacrosTests JasmineConsumerTests WfDriverTests)
  set_target_properties(tests PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE ${WEBFRONT_TEST_UNITY_BATCH})
  foreach(_source IN LISTS TESTS_LIST)
    cmake_path(GET _source STEM _stem)
    if(NOT _stem IN_LIST _heavy)
      set_source_files_properties(${_source} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    endif()
  endforeach()
endif()
configure_file(Version.hpp.in generated/Version.hpp @ONLY)
target_include_directories(tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# GCC reports -Wnonnull and -Wnull-dereference only when it optimizes, so the Debug CI job missed the false positive of
# a Release build in the React store (#429; -O3, as in Release, is needed for the full test). Compile that test once
# more at -O3; WebFront_options is left out because its coverage flags would restore -O0.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_library(react_store_optimized OBJECT ReactStoreTests.cpp)
  target_link_libraries(react_store_optimized PRIVATE WebFront_test_hardening WebFront_warnings WebFront)
  target_compile_options(react_store_optimized PRIVATE -O3)
endif()

# Labels select the tiers (docs/TEST_TIERS.md): fast unit tests, browser integration, portability across toolchains.
# One CTest case per scenario starts one process per scenario: more than 500, which is most of the run time of the unit
# tests (docs/TEST_PERFORMANCE.md). WEBFRONT_TEST_UNIT_SHARDS runs them as that many processes instead, each a
# shard that writes the result of every scenario to a JUnit report. The default keeps one case per scenario, which
# `ctest -R` filters by name.
set(WEBFRONT_TEST_UNIT_SHARDS 0 CACHE STRING "Run the unit tests as this many CTest cases; 0 registers one case per scenario")
if(NOT WEBFRONT_TEST_UNIT_SHARDS MATCHES "^[0-9]+$")
  message(FATAL_ERROR "WEBFRONT_TEST_UNIT_SHARDS must be a non-negative integer, not '${WEBFRONT_TEST_UNIT_SHARDS}'")
endif()
if(WEBFRONT_TEST_UNIT_SHARDS GREATER 0)
  # The shards split the scenarios in their declaration order, which every process sees alike. The scenarios of a shard
  # share its time limit; the slowest of them takes a few seconds.
  webfront_bdd_discover_tests(tests SHARDS ${WEBFRONT_TEST_UNIT_SHARDS} SHARD_NAME UnitTests
      JUNIT_PREFIX "${CMAKE_BINARY_DIR}/unit-results" PROPERTIES TIMEOUT ${_shard_timeout} LABELS fast ${_unit_test_environment})
else()
  webfront_bdd_discover_tests(tests PROPERTIES TIMEOUT ${_unit_timeout} LABELS fast ${_unit_test_environment})
endif()

# The developer journey type-checks the declaration this tool writes from the hand-described settings (#383).
add_executable(forms_declaration FormsDeclaration.cpp)
target_link_libraries(forms_declaration PRIVATE WebFront_test_hardening WebFront_warnings WebFront_options WebFront)

# The optional C++26 layer of the forms (ADR-018, #383): reflection needs GCC 16 with -std=c++26 -freflection. The library
# stays C++23; only this test executable is compiled as C++26. CI requires it on Linux so that it cannot be skipped there.
option(WEBFRONT_REQUIRE_REFLECTION_TESTS "Fail the configuration when the C++26 reflection tests cannot be compiled" OFF)
set(_reflection_flags "-std=c++26 -freflection")
set(_reflection_supported OFF)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  include(CheckCXXSourceCompiles)
  # The probe sets the standard as the test target does: a CMAKE_CXX_STANDARD of the project (23 under WEBFRONT_USE_MDDLOG)
  # would otherwise add its own -std=c++23 after CMAKE_REQUIRED_FLAGS and hide reflection.
  # WEBFRONT_HAS_REFLECTION is a cache entry, so it leaves the block.
  block(SCOPE_FOR VARIABLES)
  set(CMAKE_CXX_STANDARD 26)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_REQUIRED_FLAGS "${_reflection_flags}")
  check_cxx_source_compiles([=[
#include <meta>
#if !defined(__cpp_impl_reflection) || !defined(__cpp_expansion_statements)
#error no reflection
#endif
struct S { [[=1]] int m; };
static_assert(std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()).size() == 1);
int main() { return 0; }
]=] WEBFRONT_HAS_REFLECTION)
  endblock()
  set(_reflection_supported ${WEBFRONT_HAS_REFLECTION})
endif()
if(_reflection_supported)
  # Bdd.hpp is a header: the whole executable, its framework included, is compiled as C++26.
  add_executable(forms_reflection_tests Main.cpp FormsReflectionTests.cpp)
  target_link_libraries(forms_reflection_tests PRIVATE WebFront::Bdd WebFront_test_hardening WebFront_warnings WebFront_options WebFront)
  set_target_properties(forms_reflection_tests PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_options(forms_reflection_tests PRIVATE -freflection)
  webfront_bdd_discover_tests(forms_reflection_tests TEST_PREFIX "C++26." PROPERTIES TIMEOUT 60 LABELS fast)
  # Misplaced annotations do not compile: each case must fail with the message of describe<T>().
  foreach(_case IN ITEMS "RANGE_ON_BOOL|Range applies to numeric members only" "RANGE_ON_STRING|Range applies to numeric members only"
                         "RANGE_ON_ENUM|Range applies to numeric members only" "REPEATED_LABEL|Label is repeated on a member"
                         "REPEATED_RANGE|Range is repeated on a member")
    string(REPLACE "|" ";" _case "${_case}")
    list(GET _case 0 _name)
    list(GET _case 1 _message)
    add_test(NAME C++26.FormsReflectionRejects.${_name}
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -freflection -fsyntax-only -I${PROJECT_SOURCE_DIR}/include
            -DREJECT_${_name} ${CMAKE_CURRENT_SOURCE_DIR}/FormsReflectionRejects.cpp)
    set_tests_properties(C++26.FormsReflectionRejects.${_name} PROPERTIES PASS_REGULAR_EXPRESSION "${_message}" TIMEOUT 60 LABELS fast)
  endforeach()
  # Without a misplaced annotation the same file compiles: the cases above fail only on their assertion.
  add_test(NAME C++26.FormsReflectionRejects.NONE COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 -freflection -fsyntax-only
      -I${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/FormsReflectionRejects.cpp)
  set_tests_properties(C++26.FormsReflectionRejects.NONE PROPERTIES TIMEOUT 60 LABELS fast)
elseif(WEBFRONT_REQUIRE_REFLECTION_TESTS)
  message(FATAL_ERROR "WEBFRONT_REQUIRE_REFLECTION_TESTS: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
      "does not compile C++26 reflection with ${_reflection_flags} (GCC 16 or later is required)")
else()
  message(STATUS "Skipping the C++26 reflection tests of the forms: GCC 16 with -freflection is required")
endif()

add_test(NAME ReleaseVersionConsistency
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseVersion.cmake)
add_test(NAME ReleaseVersionRegression
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/release-version-regression
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseVersionTests.cmake)
add_test(NAME DeveloperPinConsistency
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckDeveloperPin.cmake)
add_test(NAME DeveloperPinRegression
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/developer-pin-regression
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckDeveloperPinTests.cmake)
add_test(NAME ReleaseDeveloperWorkflow
    COMMAND ${CMAKE_COMMAND} -DWORKFLOW=${PROJECT_SOURCE_DIR}/.github/workflows/ReleaseDeveloper.yml
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseWorkflow.cmake)
add_test(NAME ReleaseDeveloperWorkflowRegression
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/release-workflow-regression
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleaseWorkflowTests.cmake)
add_test(NAME SemVerOrdering
    COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckSemVer.cmake)
add_test(NAME DeveloperCompatibilityTable
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/developer-compatibility
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckDeveloperCompatibility.cmake)
set_tests_properties(DeveloperPinConsistency DeveloperPinRegression ReleaseDeveloperWorkflow ReleaseDeveloperWorkflowRegression
    DeveloperCompatibilityTable SemVerOrdering
    PROPERTIES TIMEOUT 60 LABELS fast)
add_test(NAME BootstrapProvisioning
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-provisioning
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckBootstrapProvisioning.cmake)

# Windows bash.exe may be the WSL launcher rather than a native Bash runtime.
if(UNIX)
    add_test(NAME RunWithDeadline COMMAND ${CMAKE_COMMAND} -DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/RunWithDeadline.sh
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckRunWithDeadline.cmake)
    set_tests_properties(RunWithDeadline PROPERTIES TIMEOUT 60 LABELS fast)
    # The closed list of paths of the developer release commit (ADR-021).
    add_test(NAME ReleasePathsClosedList
        COMMAND ${CMAKE_COMMAND} -DSCRIPT=${PROJECT_SOURCE_DIR}/js/developer/tools/check-release-paths.sh
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/release-paths -DBASH=bash -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReleasePaths.cmake)
    set_tests_properties(ReleasePathsClosedList PROPERTIES TIMEOUT 60 LABELS fast)
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

if(WEBFRONT_ENABLE_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()

# Replaces the global operator new to count allocations: kept out of the unit-test executable and TSan builds.
# Clang's TSan C++ runtime defines the same operators, so this executable cannot link with it.
# Ordinary and ASan/UBSan builds retain this regression; the rest of the suite still runs under TSan.
if(NOT WEBFRONT_ENABLE_SANITIZER_THREAD)
  add_executable(frame_allocation FrameAllocationTest.cpp)
  target_link_libraries(frame_allocation PRIVATE WebFront_test_hardening WebFront_warnings WebFront_options WebFront)
  add_test(NAME FrameBufferNotFormattedWhenDebugDisabled COMMAND frame_allocation)
  set_tests_properties(FrameBufferNotFormattedWhenDebugDisabled PROPERTIES TIMEOUT 30)
endif()

if(WEBFRONT_USE_MDDLOG)
  # This consumer includes only the public header and links only the facade target.
  add_executable(mddlog_header_consumer MddlogHeaderConsumer.cpp)
  target_link_libraries(mddlog_header_consumer PRIVATE WebFront::mddlog)
  set_target_properties(mddlog_header_consumer PROPERTIES CXX_SCAN_FOR_MODULES OFF CXX_MODULE_STD OFF)
  add_test(NAME MddlogHeaderConsumer COMMAND mddlog_header_consumer)
endif()

add_test(NAME ReactProjectPreparation COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/react-project-preparation -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckPrepareReactProject.cmake)
set_tests_properties(ReactProjectPreparation PROPERTIES TIMEOUT 30)

# The documented newcomer commands and snippets must keep matching the built examples and CI.
add_test(NAME RepositoryLayout
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckRepositoryLayout.cmake)
add_test(NAME RepositoryLayoutRejectsVendoredSources
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/layout-negative
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckRepositoryLayoutNegative.cmake)
add_test(NAME NewcomerDocsMatchExamples
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckNewcomerDocs.cmake)
# What an agent reads first states the development model and how it runs wf itself (#561).
add_test(NAME AgentGuidance
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/agent-guidance
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckAgentGuide.cmake)
set_tests_properties(AgentGuidance PROPERTIES TIMEOUT 30 LABELS fast)
add_test(NAME WebFrontDistribution
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/distribution
        -DCONSUMER_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DCONSUMER_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCONSUMER_CONFIG=$<CONFIG>
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckWebFrontDistribution.cmake)
set_tests_properties(WebFrontDistribution PROPERTIES TIMEOUT 120)
add_test(NAME BuildToolRequirements
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build-tool-requirements
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckBuildToolRequirements.cmake)
set_tests_properties(BuildToolRequirements PROPERTIES TIMEOUT 30 LABELS fast)
add_test(NAME ChangelogFormat
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckChangelogFormat.cmake)
set_tests_properties(ChangelogFormat PROPERTIES TIMEOUT 30 LABELS fast)
add_test(NAME ModuleScanWarning
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/module-scan
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckModuleScan.cmake)
set_tests_properties(ModuleScanWarning PROPERTIES TIMEOUT 120 LABELS fast)
add_test(NAME FrontendSelection
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckFrontendSelection.cmake)
add_test(NAME OptionalWindowDisplay COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckOptionalWindowDisplay.cmake)
add_test(NAME NativeAutodetectReconfiguration
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-autodetect
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckNativeAutodetect.cmake)
# The test takes about 5 s; the wide bound only absorbs a loaded runner (one unexplained stall at 60 s, #416).
set_tests_properties(NativeAutodetectReconfiguration PROPERTIES TIMEOUT 180)
# WebFront's prefixed test options, the migration of the former names and their isolation from a parent (#412).
add_test(NAME TestOptionsConfiguration
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-options
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckTestOptions.cmake)
set_tests_properties(TestOptionsConfiguration PROPERTIES TIMEOUT 120 LABELS fast)
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
# Exercise public frontend helpers across directory scopes and the default detection in a real parent project.
add_test(NAME FrontendConsumerConfiguration
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/frontend-consumer
        -DCPM_MODULE=${CPM_DOWNLOAD_LOCATION}
        "-DDEPENDENCY_ARGS=${_consumer_dependency_args}"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckFrontendConsumer.cmake)
set_tests_properties(FrontendConsumerConfiguration PROPERTIES TIMEOUT 120)
# Read the actual target's launcher at generation time, including an explicitly
# configured or auto-discovered cache. Do not depend on Cache.cmake's local variable.
# The build smoke test opts out of detection to avoid SDK downloads after configuration;
# FrontendConsumerConfiguration tests the automatic path with deterministic support instead.
add_test(NAME AddSubdirectoryConsumer
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${PROJECT_SOURCE_DIR}/test/cmake/consumer ${CMAKE_CURRENT_BINARY_DIR}/consumer-build
        --build-generator ${CMAKE_GENERATOR}
        --build-config $<CONFIG>
        --build-options -DWEBFRONT_SOURCE_DIR=${PROJECT_SOURCE_DIR} -DCPM_MODULE=${CPM_DOWNLOAD_LOCATION} ${_consumer_dependency_args}
            -DWEBFRONT_AUTODETECT_NATIVE=OFF
            -DENABLE_CACHE=OFF "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=$<TARGET_PROPERTY:tests,CXX_COMPILER_LAUNCHER>"
            -DCMAKE_BUILD_TYPE=$<CONFIG> -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
set_tests_properties(AddSubdirectoryConsumer PROPERTIES TIMEOUT 600 LABELS portability)

# Compile the real native frontend from a consuming project on the native desktop CI jobs.
# Reuse the configured SDK source (including WebView2) and CPM bootstrap without network access.
webfront_native_frontend_option(_consumer_native_option)
set(_consumer_native_enabled OFF)
if(_consumer_native_option)
    set(_consumer_native_enabled "${${_consumer_native_option}}")
endif()
if(_consumer_native_enabled)
    set(_consumer_native_dependency_args ${_consumer_dependency_args})
    if(WEBFRONT_EMBED_WEBVIEW2)
        list(APPEND _consumer_native_dependency_args -DCPM_WebView2SDK_SOURCE=${WebView2SDK_SOURCE_DIR})
    endif()
    add_test(NAME AddSubdirectoryNativeConsumer
        COMMAND ${CMAKE_CTEST_COMMAND}
            --build-and-test ${PROJECT_SOURCE_DIR}/test/cmake/consumer ${CMAKE_CURRENT_BINARY_DIR}/native-consumer-build
            --build-generator ${CMAKE_GENERATOR} --build-config $<CONFIG>
            --build-options -DWEBFRONT_SOURCE_DIR=${PROJECT_SOURCE_DIR} -DCPM_MODULE=${CPM_DOWNLOAD_LOCATION}
                ${_consumer_native_dependency_args} -DEXPECT_NATIVE=ON
                -DENABLE_CACHE=OFF "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=$<TARGET_PROPERTY:tests,CXX_COMPILER_LAUNCHER>"
                -DCMAKE_BUILD_TYPE=$<CONFIG> -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
    set_tests_properties(AddSubdirectoryNativeConsumer PROPERTIES TIMEOUT 600)
endif()

# The cases that configure a CMake project test the build system, not the C++ of the library: a pull request runs them only
# when the build system changes (docs/TEST_TIERS.md). None of them compiles library code, which is why
# AddSubdirectoryNativeConsumer is not among them. `configuration` comes in addition to a tier label.
foreach(_case IN ITEMS ModuleScanWarning TestOptionsConfiguration NativeAutodetectReconfiguration FrontendSelection
        FrontendSelectionMultiConfig CEFTestingState CEFCopyDestinations WebView2UnsupportedToolchain WebView2LoaderArchitecture
        WKWebViewConfiguration FrontendConsumerConfiguration TestSuiteRepetition)
    if(TEST ${_case})
        set_property(TEST ${_case} APPEND PROPERTY LABELS configuration)
    endif()
endforeach()

# Manual timing evidence for gzip cache/cold-decode work; never a timing-based test.
add_executable(gzip_cache_benchmark EXCLUDE_FROM_ALL GzipCacheBenchmark.cpp)
target_link_libraries(gzip_cache_benchmark PRIVATE WebFront_test_hardening WebFront_warnings WebFront_options WebFront)

add_test(NAME TransportBenchmarksAreManual COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckManualTransportBenchmarks.cmake)

if(NOT WEBFRONT_ENABLE_SANITIZER_THREAD)
  add_executable(gzip_decoder_benchmark EXCLUDE_FROM_ALL GzipDecoderBenchmark.cpp)
  target_link_libraries(gzip_decoder_benchmark PRIVATE WebFront_test_hardening WebFront_warnings WebFront_options WebFront)
endif()

if(WEBFRONT_GZIP_DIFFERENTIAL)
  find_package(ZLIB REQUIRED)
  add_executable(gzip_differential ../fuzz/GzipDifferential.cpp)
  target_link_libraries(gzip_differential PRIVATE WebFront_test_hardening WebFront_options WebFront ZLIB::ZLIB)
  add_test(NAME GzipZlibDifferential COMMAND gzip_differential)
  set_tests_properties(GzipZlibDifferential PROPERTIES TIMEOUT 60)
endif()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME FuzzerConcurrency COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/fuzz/TestRunFuzzers.py")
    add_test(NAME TestFlakeReport COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/tools/TestFlakeReport.py")
    add_test(NAME HardwareWorkflows COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/tools/TestHardwareWorkflows.py")
    set_tests_properties(FuzzerConcurrency TestFlakeReport HardwareWorkflows PROPERTIES TIMEOUT 60 LABELS fast)
endif()

# Automation does not exist in a distribution build (ADR-025): Enable.hpp, which opens the MCP listener, refuses to compile
# with WEBFRONT_DISTRIBUTION. The test builds that translation unit and expects the compiler to name the reason.
add_library(automation_distribution_guard OBJECT EXCLUDE_FROM_ALL AutomationDistributionGuard.cpp)
target_link_libraries(automation_distribution_guard PRIVATE WebFront WebFront_options)
target_compile_definitions(automation_distribution_guard PRIVATE WEBFRONT_DISTRIBUTION=1)
add_test(NAME AutomationAbsentFromDistribution
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target automation_distribution_guard --config $<CONFIG>)
set_tests_properties(AutomationAbsentFromDistribution PROPERTIES PASS_REGULAR_EXPRESSION "does not exist in a distribution build")

# A project outside WebFront finds the installed WebFrontBdd package, writes scenarios and has CTest discover them (#647).
add_test(NAME BddConsumer COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DBUILD_DIR=${CMAKE_BINARY_DIR}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/bdd-consumer "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    -DCONFIG=$<CONFIG> -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckBddConsumer.cmake)
set_tests_properties(BddConsumer PROPERTIES TIMEOUT 300 LABELS portability)
