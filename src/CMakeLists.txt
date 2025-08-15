find_package(Threads)

add_executable(WebFrontApp HelloWorld.cpp)

# CEF availability is determined by the WEBFRONT_EMBED_CEF option
set(CEF_AVAILABLE OFF)
if(WEBFRONT_EMBED_CEF AND TARGET cef AND NOT CEF_WRAPPER_BUILD_SKIP)
    set(CEF_AVAILABLE ON)
endif()

# Debug: Check what the cef target provides and if CEF is available
if(CEF_AVAILABLE)
    message(STATUS "CEF target available")
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
        if(WIN32)
            target_link_libraries(WebFrontApp PRIVATE 
                "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Debug/libcef.lib"
                comctl32.lib rpcrt4.lib shlwapi.lib ws2_32.lib winmm.lib winspool.lib psapi.lib
            )
        else()
            # For non-Windows Debug builds, use the cef target as-is
            target_link_libraries(WebFrontApp PRIVATE cef)
        endif()
    else()
        # For Release builds, use the cef target as-is
        target_link_libraries(WebFrontApp PRIVATE cef)
    endif()
    
    # Define that CEF is available for conditional compilation
    target_compile_definitions(WebFrontApp PRIVATE WEBFRONT_EMBED_CEF)
else()
    message(STATUS "CEF target does NOT exist - CEF features will be disabled")
endif()

# Link the CEF wrapper library if it exists
if(TARGET libcef_dll_wrapper)
    target_link_libraries(WebFrontApp PRIVATE libcef_dll_wrapper)
endif()

# Link the main CEF framework directly if CEF target exists
if(TARGET cef)
    target_link_libraries(WebFrontApp PRIVATE cef)
    message(STATUS "Linking CEF framework to WebFrontApp")
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
            # Create the Frameworks directory in the parent of the target file directory
            add_custom_command(TARGET WebFrontApp POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:WebFrontApp>/../Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework"
                    "$<TARGET_FILE_DIR:WebFrontApp>/../Frameworks/Chromium Embedded Framework.framework"
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
                "${CEF_BUILD_DIR}/v8_context_snapshot.bin"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMENT "Copying CEF runtime files to output directory"
        )
    endif()
      # Copy CEF Resources
    if(WIN32)
        # Windows uses Resources directory, not framework structure
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
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_ROOT}/Resources/locales"
                "$<TARGET_FILE_DIR:WebFrontApp>/locales"
            COMMENT "Copying CEF resource files to output directory"
        )
    elseif(APPLE)
        # macOS framework resources
        add_custom_command(TARGET WebFrontApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework/Resources/chrome_100_percent.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework/Resources/chrome_200_percent.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework/Resources/icudtl.dat"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework/Resources/resources.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMENT "Copying CEF resource files to output directory"
        )
    elseif(UNIX)
        # Linux uses Resources directory like Windows
        add_custom_command(TARGET WebFrontApp POST_BUILD
            # Copy to application directory (existing behavior)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/chrome_100_percent.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/chrome_200_percent.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/icudtl.dat"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/resources.pak"
                "$<TARGET_FILE_DIR:WebFrontApp>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/locales"
                "$<TARGET_FILE_DIR:WebFrontApp>/locales"
            # Also copy to CEF binary directory where CEF expects them (fix ICU loading issue)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/chrome_100_percent.pak"
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Release/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/chrome_200_percent.pak"
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Release/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/icudtl.dat"
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Release/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/resources.pak"
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Release/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Resources/locales"
                "${CMAKE_CURRENT_BINARY_DIR}/../_deps/cef_binaries-src/Release/locales"
            COMMENT "Copying CEF resource files to output and binary directories"
        )
    endif()
    
    # Note: locales are now embedded within the framework as .pak files in .lproj directories
    # No separate locales directory copying needed for this CEF version
endif()

add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})