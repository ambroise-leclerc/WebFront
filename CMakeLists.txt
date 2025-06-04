cmake_minimum_required (VERSION 3.18)

set(VERSION_STRING_REGEX "inline constexpr std::string_view version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_STRING_REGEX})
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

add_library(WebFront INTERFACE)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(StandardProjectSettings)
include(PreventInSourceBuilds)
add_library(WebFront_options INTERFACE)
target_compile_features(WebFront_options INTERFACE cxx_std_23)
if(APPLE)
  target_compile_definitions(WebFront_options INTERFACE __cpp_lib_format=202207L)
endif()

add_library(WebFront_warnings INTERFACE)

include(Cache)

include(Linker)
configure_linker(WebFront_options)

include(CompilerWarnings)
set_project_warnings(WebFront_warnings)

include(Sanitizers)
enable_sanitizers(WebFront_options)

configure_file(Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile)
include(Doxygen)
enable_doxygen()

include(StaticAnalyzers)
include(CPM)

CPMAddPackage("gh:ambroise-leclerc/networking-ts-impl@1.12.0a")
#CPMAddPackage("gh:ambroise-leclerc/CEF@118.7.1c")
CPMAddPackage(
  NAME cef
  GITHUB_REPOSITORY ambroise-leclerc/CEF
  GIT_TAG 17-cef-v137
)


# Create our own CEF wrapper library target
set(CEF_ROOT "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src")
if(EXISTS "${CEF_ROOT}/libcef_dll")
  # Create a simple library target for CEF wrapper
  file(GLOB_RECURSE CEF_WRAPPER_SOURCES 
    "${CEF_ROOT}/libcef_dll/*.cc"
    "${CEF_ROOT}/libcef_dll/*.cpp"
  )
    if(CEF_WRAPPER_SOURCES)
    add_library(libcef_dll_wrapper STATIC ${CEF_WRAPPER_SOURCES})
    target_include_directories(libcef_dll_wrapper PUBLIC "${CEF_ROOT}")
    target_include_directories(libcef_dll_wrapper PUBLIC "${CEF_ROOT}/include")
    
    # Set basic properties
    set_target_properties(libcef_dll_wrapper PROPERTIES PREFIX "")
    target_compile_definitions(libcef_dll_wrapper PRIVATE WRAPPING_CEF_SHARED)
    
    # Force C++17 for CEF wrapper to ensure compatibility
    target_compile_features(libcef_dll_wrapper PRIVATE cxx_std_17)
    
    # Add Windows-specific compiler flags for CEF
    if(WIN32)
      target_compile_options(libcef_dll_wrapper PRIVATE
        /MP           # Multiprocess compilation
        /wd4100       # Ignore "unreferenced formal parameter" warning
        /wd4127       # Ignore "conditional expression is constant" warning
        /wd4244       # Ignore "conversion possible loss of data" warning
        /wd4996       # Ignore "function or variable may be unsafe" warning
      )
      target_compile_definitions(libcef_dll_wrapper PRIVATE
        WIN32 _WIN32 _WINDOWS
        UNICODE _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _HAS_EXCEPTIONS=0
      )
    endif()
  endif()
endif()

target_link_libraries(WebFront INTERFACE WebFront_options WebFront_warnings)
target_include_directories(WebFront INTERFACE ${CMAKE_SOURCE_DIR}/include)
# Ajouter les headers CEF à WebFront comme headers système pour éviter les warnings
if(EXISTS "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}/include")
endif()
include_directories(SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
option(ENABLE_TESTING "Enable Test Builds" ON)
option(ENABLE_FUZZING "Enable Fuzzing Builds" OFF)

add_compile_definitions(_WIN32_WINNT=0x0501)

if(ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test)
  add_subdirectory(webtest)
endif()

add_subdirectory(src)
