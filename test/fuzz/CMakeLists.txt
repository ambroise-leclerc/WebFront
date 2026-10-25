if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
    message(FATAL_ERROR "ENABLE_FUZZING requires Clang with libFuzzer (Linux or macOS)")
endif()
if(ENABLE_SANITIZER_THREAD OR ENABLE_SANITIZER_MEMORY)
    message(FATAL_ERROR "Fuzzing uses ASan+UBSan and cannot be combined with TSan or MSan")
endif()

include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
check_cxx_source_compiles("#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }"
    WEBFRONT_HAS_LIBFUZZER)
cmake_pop_check_state()
if(NOT WEBFRONT_HAS_LIBFUZZER)
    message(FATAL_ERROR "ENABLE_FUZZING requires the compiler-rt libFuzzer, ASan and UBSan runtimes")
endif()

foreach(_decoder IN ITEMS Messages HTTP WebSocket Gzip)
    string(TOLOWER "${_decoder}" _corpus)
    add_executable(fuzz_${_corpus} ${_decoder}Fuzz.cpp)
    target_link_libraries(fuzz_${_corpus} PRIVATE WebFront WebFront_test_hardening)
    target_compile_options(fuzz_${_corpus} PRIVATE -g -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz_${_corpus} PRIVATE -fsanitize=fuzzer,address,undefined)
    # A writable copy lets libFuzzer evolve its corpus without touching the checkout.
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_corpus}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/corpus")
endforeach()

if(WEBFRONT_GZIP_DIFFERENTIAL)
    find_package(ZLIB REQUIRED)
    add_executable(fuzz_gzip_differential GzipDifferentialFuzz.cpp)
    target_link_libraries(fuzz_gzip_differential PRIVATE WebFront WebFront_test_hardening ZLIB::ZLIB)
    target_compile_options(fuzz_gzip_differential PRIVATE -g -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz_gzip_differential PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
