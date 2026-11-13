# The CUDA backend of the Life CUDA example (examples/life/cuda/). CUDA is optional: without a usable toolkit the
# example, its tests and the browser spec are built on the CPU kernel alone, which is the same code.
#
# nvcc does not compile C++23, so the kernels live in one .cu unit that includes no WebFront header and is compiled
# at C++17. nvcc also bounds the host compiler it accepts (CUDA 12.6 refuses GCC newer than 13): when the default
# host compiler is refused, CMAKE_CUDA_HOST_COMPILER (or the CUDAHOSTCXX environment variable) names another one.
set(WEBFRONT_LIFE_CUDA "AUTO" CACHE STRING "CUDA backend of the Life CUDA example: AUTO (when a working toolkit is found), ON or OFF")
set_property(CACHE WEBFRONT_LIFE_CUDA PROPERTY STRINGS AUTO ON OFF)

# A macro: enable_language() must run in the scope of the top-level CMakeLists.txt, above every directory whose
# targets link the CUDA unit.
macro(webfront_enable_life_cuda)
    if(NOT WEBFRONT_LIFE_CUDA MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "WEBFRONT_LIFE_CUDA must be AUTO, ON or OFF, not '${WEBFRONT_LIFE_CUDA}'")
    endif()
    set(WEBFRONT_LIFE_CUDA_ENABLED OFF)
    if(WEBFRONT_LIFE_CUDA STREQUAL "ON")
        enable_language(CUDA)
        set(WEBFRONT_LIFE_CUDA_ENABLED ON)
    elseif(WEBFRONT_LIFE_CUDA STREQUAL "AUTO")
        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            enable_language(CUDA)
            set(WEBFRONT_LIFE_CUDA_ENABLED ON)
        endif()
    endif()
    if(WEBFRONT_LIFE_CUDA_ENABLED)
        message(STATUS "Life CUDA example: CUDA ${CMAKE_CUDA_COMPILER_VERSION} backend enabled (${CMAKE_CUDA_COMPILER})")
    else()
        message(STATUS "Life CUDA example: CPU kernel only (WEBFRONT_LIFE_CUDA=${WEBFRONT_LIFE_CUDA}, no working CUDA compiler)")
    endif()

    # What a target computing Life fields links: nothing on a CPU-only build, the CUDA unit otherwise.
    add_library(WebFront_life_field INTERFACE)
    if(WEBFRONT_LIFE_CUDA_ENABLED)
        add_library(WebFront_life_field_cuda STATIC "${PROJECT_SOURCE_DIR}/examples/life/cuda/LifeField.cu")
        target_compile_features(WebFront_life_field_cuda PRIVATE cuda_std_17)
        target_compile_definitions(WebFront_life_field_cuda PUBLIC WEBFRONT_LIFE_CUDA=1)
        set_target_properties(WebFront_life_field_cuda PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_libraries(WebFront_life_field INTERFACE WebFront_life_field_cuda)
    endif()
endmacro()
