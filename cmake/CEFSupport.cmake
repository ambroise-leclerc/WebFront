# ==============================================================================
# CEFSupport.cmake - Chromium Embedded Framework Integration Module
# ==============================================================================
# 
# This module provides comprehensive CEF (Chromium Embedded Framework) support
# including target configuration, resource management, and platform-specific
# file deployment.
#
# Usage:
#   include(CEFSupport)
#   setup_cef_for_target(target_name)
#
# Functions provided:
#   - setup_cef_for_target(target_name)  : Complete CEF setup for a target
#   - configure_cef_for_target(target_name) : Configure CEF linking and compilation
#   - copy_cef_runtime_files(target_name build_dir) : Copy platform runtime files
#   - copy_cef_resource_files(target_name) : Copy CEF resource files (with ICU fix)
#
# ==============================================================================

# Guard against multiple inclusion
if(CEF_SUPPORT_INCLUDED)
    return()
endif()
set(CEF_SUPPORT_INCLUDED TRUE)

# ==============================================================================
# CEF Configuration Constants
# ==============================================================================

# CEF resource files that need to be deployed
set(CEF_RESOURCE_FILES
    "chrome_100_percent.pak"
    "chrome_200_percent.pak" 
    "icudtl.dat"
    "resources.pak"
)

# Platform-specific CEF runtime files
set(CEF_WINDOWS_RUNTIME_FILES
    "libcef.dll"
    "chrome_elf.dll"
    "d3dcompiler_47.dll"
    "libEGL.dll"
    "libGLESv2.dll"
    "vk_swiftshader.dll"
    "vulkan-1.dll"
    "v8_context_snapshot.bin"
    "vk_swiftshader_icd.json"
)

set(CEF_LINUX_RUNTIME_FILES
    "libcef.so"
    "v8_context_snapshot.bin"
)

# ==============================================================================
# Private Utility Functions
# ==============================================================================

# Internal function to copy a list of files from source to destination directory
function(_cef_copy_files_to_target target_name source_dir dest_dir file_list comment_text)
    set(copy_commands "")
    foreach(file_name IN LISTS file_list)
        list(APPEND copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${source_dir}/${file_name}"
                "${dest_dir}"
        )
    endforeach()
    
    add_custom_command(TARGET ${target_name} POST_BUILD
        ${copy_commands}
        COMMENT "${comment_text}"
    )
endfunction()

# Internal function to copy directory with proper error handling
function(_cef_copy_directory_to_target target_name source_dir dest_dir comment_text)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${source_dir}"
            "${dest_dir}"
        COMMENT "${comment_text}"
    )
endfunction()

# Internal function to setup CEF paths and variables
function(_cef_setup_paths)
    set(CEF_ROOT "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src" PARENT_SCOPE)
    
    # Determine build directory based on configuration
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CEF_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Debug" PARENT_SCOPE)
    else()
        set(CEF_BUILD_DIR "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Release" PARENT_SCOPE)
    endif()
    
    # Common resource paths
    set(CEF_RESOURCE_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Resources" PARENT_SCOPE)
    set(CEF_BINARY_RELEASE_DIR "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Release" PARENT_SCOPE)
endfunction()

# ==============================================================================
# Public API Functions
# ==============================================================================

#.rst:
# .. command:: configure_cef_for_target
#
#    Configure CEF compilation and linking for a target::
#
#      configure_cef_for_target(<target>)
#
#    This function:
#    - Checks CEF availability and configuration
#    - Sets up include directories with SYSTEM flag to suppress warnings
#    - Configures appropriate linking based on build type and platform
#    - Links CEF wrapper library if available
#    - Defines WEBFRONT_EMBED_CEF compilation flag
#
function(configure_cef_for_target target_name)
    # CEF availability check
    set(CEF_AVAILABLE OFF)
    if(WEBFRONT_EMBED_CEF AND TARGET cef AND NOT CEF_WRAPPER_BUILD_SKIP)
        set(CEF_AVAILABLE ON)
    endif()

    if(NOT CEF_AVAILABLE)
        message(STATUS "CEF target does NOT exist - CEF features will be disabled for ${target_name}")
        return()
    endif()

    message(STATUS "Configuring CEF for target: ${target_name}")
    
    # Debug information
    get_target_property(CEF_INCLUDE_DIRS cef INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(CEF_LINK_LIBS cef INTERFACE_LINK_LIBRARIES)
    message(STATUS "CEF include directories: ${CEF_INCLUDE_DIRS}")
    message(STATUS "CEF link libraries: ${CEF_LINK_LIBS}")

    # Configure include directories - use SYSTEM to suppress CEF header warnings
    target_include_directories(${target_name} SYSTEM PRIVATE 
        "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src"
    )

    # Configure linking based on build type and platform
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
        message(STATUS "Using Debug CEF libraries for ${target_name}")
        target_link_libraries(${target_name} PRIVATE 
            "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/Debug/libcef.lib"
            comctl32.lib rpcrt4.lib shlwapi.lib ws2_32.lib winmm.lib winspool.lib psapi.lib
        )
    else()
        target_link_libraries(${target_name} PRIVATE cef)
    endif()

    # Link wrapper library if available
    if(TARGET libcef_dll_wrapper)
        target_link_libraries(${target_name} PRIVATE libcef_dll_wrapper)
        message(STATUS "Linked CEF wrapper library to ${target_name}")
    endif()

    # Define CEF availability for compilation
    target_compile_definitions(${target_name} PRIVATE WEBFRONT_EMBED_CEF)
    
    message(STATUS "CEF configuration completed for ${target_name}")
endfunction()

#.rst:
# .. command:: copy_cef_runtime_files
#
#    Copy CEF runtime files (shared libraries, binaries) to target directory::
#
#      copy_cef_runtime_files(<target> <cef_build_dir>)
#
#    This function copies platform-specific CEF runtime files:
#    - Windows: DLLs and supporting files
#    - macOS: Framework bundle 
#    - Linux: Shared libraries
#
function(copy_cef_runtime_files target_name cef_build_dir)
    set(target_dir "$<TARGET_FILE_DIR:${target_name}>")
    
    if(WIN32)
        _cef_copy_files_to_target(${target_name} 
            "${cef_build_dir}" 
            "${target_dir}"
            "${CEF_WINDOWS_RUNTIME_FILES}"
            "Copying CEF runtime files to output directory"
        )
    elseif(APPLE)
        if(EXISTS "${cef_build_dir}/Chromium Embedded Framework.framework")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${target_dir}/../Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${cef_build_dir}/Chromium Embedded Framework.framework"
                    "${target_dir}/../Frameworks/Chromium Embedded Framework.framework"
                COMMENT "Copying CEF framework to output directory"
            )
        endif()
    elseif(UNIX)
        _cef_copy_files_to_target(${target_name}
            "${cef_build_dir}"
            "${target_dir}"
            "${CEF_LINUX_RUNTIME_FILES}"
            "Copying CEF runtime files to output directory"
        )
    endif()
    
    message(STATUS "CEF runtime files will be copied for ${target_name}")
endfunction()

#.rst:
# .. command:: copy_cef_resource_files
#
#    Copy CEF resource files (PAK files, locales, ICU data) with platform-specific logic::
#
#      copy_cef_resource_files(<target>)
#
#    This function handles:
#    - Platform-specific resource paths (Windows Resources/ vs macOS Framework/Resources/)
#    - Linux ICU loading fix (copies to both app directory and CEF binary directory)
#    - Locale directory deployment
#    - Resource PAK files (chrome_100_percent.pak, resources.pak, etc.)
#
function(copy_cef_resource_files target_name)
    set(target_dir "$<TARGET_FILE_DIR:${target_name}>")
    
    if(WIN32)
        # Windows: Copy from Resources directory
        _cef_copy_files_to_target(${target_name}
            "${CEF_ROOT}/Resources"
            "${target_dir}"
            "${CEF_RESOURCE_FILES}"
            "Copying CEF resource files to output directory"
        )
        _cef_copy_directory_to_target(${target_name}
            "${CEF_ROOT}/Resources/locales"
            "${target_dir}/locales"
            "Copying CEF locales directory"
        )
        
    elseif(APPLE)
        # macOS: Copy from framework Resources
        set(framework_resources "${CEF_BUILD_DIR}/Chromium Embedded Framework.framework/Resources")
        _cef_copy_files_to_target(${target_name}
            "${framework_resources}"
            "${target_dir}"
            "${CEF_RESOURCE_FILES}"
            "Copying CEF resource files from framework"
        )
        
    elseif(UNIX)
        # Linux: Copy to both application directory and CEF binary directory
        # This dual-copy approach fixes the ICU data loading issue where CEF
        # searches for icudtl.dat in its installation directory
        
        # Copy to application directory (standard deployment)
        _cef_copy_files_to_target(${target_name}
            "${CEF_RESOURCE_SOURCE_DIR}"
            "${target_dir}"
            "${CEF_RESOURCE_FILES}"
            "Copying CEF resource files to application directory"
        )
        _cef_copy_directory_to_target(${target_name}
            "${CEF_RESOURCE_SOURCE_DIR}/locales"
            "${target_dir}/locales"
            "Copying CEF locales to application directory"
        )
        
        # Copy to CEF binary directory (ICU loading fix)
        _cef_copy_files_to_target(${target_name}
            "${CEF_RESOURCE_SOURCE_DIR}"
            "${CEF_BINARY_RELEASE_DIR}"
            "${CEF_RESOURCE_FILES}"
            "Copying CEF resource files to binary directory (ICU fix)"
        )
        _cef_copy_directory_to_target(${target_name}
            "${CEF_RESOURCE_SOURCE_DIR}/locales"
            "${CEF_BINARY_RELEASE_DIR}/locales"
            "Copying CEF locales to binary directory (ICU fix)"
        )
    endif()
    
    message(STATUS "CEF resource files will be copied for ${target_name}")
endfunction()

#.rst:
# .. command:: setup_cef_for_target
#
#    Complete CEF setup for a target including configuration and file deployment::
#
#      setup_cef_for_target(<target>)
#
#    This is the main entry point that performs complete CEF integration:
#    - Configures CEF compilation and linking
#    - Sets up all necessary paths
#    - Copies runtime files and resources with platform-specific logic
#    - Handles the Linux ICU loading issue automatically
#
#    This function should be called once per target that needs CEF support.
#
function(setup_cef_for_target target_name)
    # Only proceed if CEF target is available
    if(NOT TARGET cef)
        message(STATUS "CEF target not available - skipping CEF setup for ${target_name}")
        return()
    endif()
    
    message(STATUS "Setting up complete CEF integration for ${target_name}")
    
    # Configure CEF for the target
    configure_cef_for_target(${target_name})
    
    # Setup internal paths
    _cef_setup_paths()
    
    # Copy runtime files and resources
    copy_cef_runtime_files(${target_name} "${CEF_BUILD_DIR}")
    copy_cef_resource_files(${target_name})
    
    message(STATUS "CEF setup completed for ${target_name}")
endfunction()

# ==============================================================================
# Module Information
# ==============================================================================

message(STATUS "CEFSupport module loaded - CEF integration functions available")

# Export function names for documentation/IDE support
set(CEF_SUPPORT_FUNCTIONS
    setup_cef_for_target
    configure_cef_for_target 
    copy_cef_runtime_files
    copy_cef_resource_files
)