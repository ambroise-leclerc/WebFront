# The optional CUDA renderer of the host-loop example (examples/host-loop/BoingCuda.cu). Without a usable toolkit the
# example and its tests are built with the CPU renderer alone, which computes the same pixels.
#
# nvcc does not compile C++23: the renderer is one .cu unit, compiled at C++17, that includes no WebFront header and
# shares BoingScene.hpp with the CPU renderer. nvcc also bounds the host compiler it accepts: when it refuses the
# default one, CMAKE_CUDA_HOST_COMPILER (or the CUDAHOSTCXX environment variable) names another.
set(WEBFRONT_HOST_LOOP_CUDA "AUTO" CACHE STRING "CUDA renderer of the host-loop example: AUTO (when a working toolkit is found), ON or OFF")
set_property(CACHE WEBFRONT_HOST_LOOP_CUDA PROPERTY STRINGS AUTO ON OFF)
set(_WEBFRONT_BOING_CUDA_DIR "${CMAKE_CURRENT_LIST_DIR}")

# A macro: enable_language() must run in the scope of the top-level CMakeLists.txt, above every directory whose
# targets link the CUDA unit. Defines WebFront_boing, which a target computing the scene links: nothing on a CPU-only
# build, the CUDA unit and WEBFRONT_BOING_CUDA otherwise.
macro(webfront_enable_host_loop_cuda)
    if(NOT WEBFRONT_HOST_LOOP_CUDA MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "WEBFRONT_HOST_LOOP_CUDA must be AUTO, ON or OFF, not '${WEBFRONT_HOST_LOOP_CUDA}'")
    endif()
    set(WEBFRONT_HOST_LOOP_CUDA_ENABLED OFF)
    # Record the choice before enable_language initializes its own compiler-dependent default.
    set(_webfront_cuda_architectures_explicit FALSE)
    if(DEFINED CMAKE_CUDA_ARCHITECTURES OR (DEFINED ENV{CUDAARCHS} AND NOT "$ENV{CUDAARCHS}" STREQUAL ""))
        set(_webfront_cuda_architectures_explicit TRUE)
    endif()
    if(WEBFRONT_HOST_LOOP_CUDA STREQUAL "ON")
        enable_language(CUDA)
        set(WEBFRONT_HOST_LOOP_CUDA_ENABLED ON)
    elseif(WEBFRONT_HOST_LOOP_CUDA STREQUAL "AUTO")
        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            enable_language(CUDA)
            set(WEBFRONT_HOST_LOOP_CUDA_ENABLED ON)
        endif()
    endif()
    if(WEBFRONT_HOST_LOOP_CUDA_ENABLED)
        if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
            include("${_WEBFRONT_BOING_CUDA_DIR}/BoingCudaArchitectures.cmake")
            webfront_cuda_select_default("${_webfront_cuda_architectures_explicit}" "${CMAKE_CUDA_COMPILER}")
        endif()
        message(STATUS "Host-loop example: CUDA ${CMAKE_CUDA_COMPILER_VERSION} renderer enabled (${CMAKE_CUDA_COMPILER})")
    else()
        message(STATUS "Host-loop example: CPU renderer only (WEBFRONT_HOST_LOOP_CUDA=${WEBFRONT_HOST_LOOP_CUDA}, no working CUDA compiler)")
    endif()

    add_library(WebFront_boing INTERFACE)
    if(WEBFRONT_HOST_LOOP_CUDA_ENABLED)
        add_library(WebFront_boing_cuda STATIC "${PROJECT_SOURCE_DIR}/examples/host-loop/BoingCuda.cu")
        target_compile_features(WebFront_boing_cuda PRIVATE cuda_std_17)
        target_compile_definitions(WebFront_boing_cuda PUBLIC WEBFRONT_BOING_CUDA=1)
        set_target_properties(WebFront_boing_cuda PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_libraries(WebFront_boing INTERFACE WebFront_boing_cuda)
    endif()
endmacro()
