find_package(Threads)

add_executable(WebFrontApp HelloWorld.cpp)

# Debug: Check what the cef target provides
if(TARGET cef)
    message(STATUS "CEF target exists")
    get_target_property(CEF_INCLUDE_DIRS cef INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(CEF_LINK_LIBS cef INTERFACE_LINK_LIBRARIES)
    message(STATUS "CEF include directories: ${CEF_INCLUDE_DIRS}")
    message(STATUS "CEF link libraries: ${CEF_LINK_LIBS}")
    # Manually add the correct include directory since the CEF target seems to have incorrect include path
    # CEF headers expect to be included from the parent of the include directory
    # Mark as SYSTEM to suppress warnings from CEF headers
    target_include_directories(WebFrontApp SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src")
    
    # Override CEF library with Debug version for Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(STATUS "Using Debug CEF libraries")
        target_link_libraries(WebFrontApp PRIVATE 
            "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Debug/libcef.lib"
            comctl32.lib rpcrt4.lib shlwapi.lib ws2_32.lib winmm.lib winspool.lib psapi.lib
        )
    else()
        # For Release builds, use the cef target as-is
        target_link_libraries(WebFrontApp PRIVATE cef)
    endif()
    
    # Define that CEF is available for conditional compilation
    target_compile_definitions(WebFrontApp PRIVATE WEBFRONT_HAS_CEF)
else()
    message(STATUS "CEF target does NOT exist - CEF features will be disabled")
endif()

# Link the CEF wrapper library if it exists
if(TARGET libcef_dll_wrapper)
    target_link_libraries(WebFrontApp PRIVATE libcef_dll_wrapper)
endif()

# Link other libraries
target_link_libraries(WebFrontApp PRIVATE 
    WebFront 
    Threads::Threads
)

# Copy CEF runtime files to output directory
if(TARGET cef)
    set(CEF_ROOT "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src")
    
    # Determine which CEF build to use based on configuration
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CEF_BUILD_DIR "${CEF_ROOT}/Debug")
    else()
        set(CEF_BUILD_DIR "${CEF_ROOT}/Release")
    endif()
    
    # Platform-specific CEF runtime file copying
    if(WIN32)
        # Copy CEF DLLs and runtime files for Windows
        add_custom_command(TARGET WebFrontApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/libcef.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/chrome_elf.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/d3dcompiler_47.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/libEGL.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/libGLESv2.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/vk_swiftshader.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/vulkan-1.dll"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/snapshot_blob.bin"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/v8_context_snapshot.bin"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/vk_swiftshader_icd.json"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMENT "Copying CEF runtime files to output directory"
        )
    elseif(APPLE)
        # Copy CEF framework and runtime files for macOS
        if(EXISTS "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework")
            add_custom_command(TARGET WebFrontApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework"
                    "$<TARGET_FILE_DIR:WebFrontApp>/Chromium Embedded Framework.framework"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CEF_BUILD_DIR}/snapshot_blob.bin"
                    "$<TARGET_FILE_DIR:WebFrontApp>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CEF_BUILD_DIR}/v8_context_snapshot.bin"
                    "$<TARGET_FILE_DIR:WebFrontApp>"
                COMMENT "Copying CEF framework and runtime files to output directory"
            )
        endif()
    elseif(UNIX)
        # Copy CEF shared libraries and runtime files for Linux
        add_custom_command(TARGET WebFrontApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/libcef.so"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/snapshot_blob.bin"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/v8_context_snapshot.bin"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMENT "Copying CEF runtime files to output directory"
        )
    endif()
    
    # Copy CEF Resources
    add_custom_command(TARGET WebFrontApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CEF_ROOT}/Resources/chrome_100_percent.pak"
            "$<TARGET_FILE_DIR:WebFrontApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CEF_ROOT}/Resources/chrome_200_percent.pak"
            "$<TARGET_FILE_DIR:WebFrontApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CEF_ROOT}/Resources/icudtl.dat"
            "$<TARGET_FILE_DIR:WebFrontApp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CEF_ROOT}/Resources/resources.pak"
            "$<TARGET_FILE_DIR:WebFrontApp>"
        COMMENT "Copying CEF resource files to output directory"
    )
    
    # Copy CEF locales directory
    add_custom_command(TARGET WebFrontApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_ROOT}/Resources/locales"
            "$<TARGET_FILE_DIR:WebFrontApp>/locales"
        COMMENT "Copying CEF locales to output directory"
    )
endif()

add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})