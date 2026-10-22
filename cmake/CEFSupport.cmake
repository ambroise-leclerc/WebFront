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
    "vk_swiftshader.dll"
    "vulkan-1.dll"
    "v8_context_snapshot.bin"
)

# Windows runtime files shipped by some CEF versions only (CEF 154 no longer ships the ANGLE DLLs)
set(CEF_WINDOWS_OPTIONAL_RUNTIME_FILES
    "libEGL.dll"
    "libGLESv2.dll"
    "dxcompiler.dll"
    "dxil.dll"
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

# Linux: CEF targets that build into one directory share it, and every CEF target shares the CEF binary directory
# (ICU fix). Copying from each target's POST_BUILD step made parallel builds copy into one directory twice at the
# same time ("Error copying directory ... locales"). Each destination gets one copy target instead, named after the
# destination, on which its targets depend. Its rules produce the copied files from their sources, so a build with
# nothing to copy copies nothing; a directory is copied with a stamp file standing for its contents.
function(_cef_shared_copy target_name destination)
    cmake_parse_arguments(PARSE_ARGV 2 copy "" "NAME;SOURCE_DIR;DIRECTORY" "FILES")
    string(MD5 key "${destination}")
    set(name webfront_cef_${copy_NAME}_${key})
    if(NOT TARGET ${name})
        set(outputs "")
        foreach(file_name IN LISTS copy_FILES)
            add_custom_command(OUTPUT "${destination}/${file_name}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${copy_SOURCE_DIR}/${file_name}" "${destination}/${file_name}"
                DEPENDS "${copy_SOURCE_DIR}/${file_name}"
                COMMENT "Copying CEF ${file_name} to ${destination}"
                VERBATIM)
            list(APPEND outputs "${destination}/${file_name}")
        endforeach()
        if(copy_DIRECTORY)
            file(GLOB_RECURSE contents CONFIGURE_DEPENDS "${copy_SOURCE_DIR}/${copy_DIRECTORY}/*")
            set(stamp "${destination}/${copy_DIRECTORY}/.webfront-copied")
            add_custom_command(OUTPUT "${stamp}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${copy_SOURCE_DIR}/${copy_DIRECTORY}" "${destination}/${copy_DIRECTORY}"
                COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
                DEPENDS ${contents}
                COMMENT "Copying CEF ${copy_DIRECTORY} to ${destination}"
                VERBATIM)
            list(APPEND outputs "${stamp}")
        endif()
        add_custom_target(${name} DEPENDS ${outputs})
    endif()
    add_dependencies(${target_name} ${name})
endfunction()

# Fails at the end of the directory if the target's output directory changed after the copy was planned for it.
function(_cef_check_output_directory target_name planned)
    _cef_output_directory(${target_name} actual)
    if(NOT actual STREQUAL planned)
        message(FATAL_ERROR "The CEF files of ${target_name} are copied to ${planned}, but it now builds into ${actual}: "
            "set its RUNTIME_OUTPUT_DIRECTORY (or CMAKE_RUNTIME_OUTPUT_DIRECTORY) before calling the WebFront frontend setup")
    endif()
endfunction()

# The directory a target's executable is built into, as $<TARGET_FILE_DIR> would name it: its RUNTIME_OUTPUT_DIRECTORY
# (initialized from CMAKE_RUNTIME_OUTPUT_DIRECTORY), else the binary directory of the directory that created it, plus
# the configuration with a multi-config generator. A path, not $<TARGET_FILE_DIR>: a copy target referencing a target
# that depends on it would form a cycle. Per-configuration output directories are refused rather than guessed.
function(_cef_output_directory target_name out_dir)
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
        string(TOUPPER "${config}" upper)
        get_target_property(per_config ${target_name} RUNTIME_OUTPUT_DIRECTORY_${upper})
        if(per_config)
            message(FATAL_ERROR "${target_name} sets RUNTIME_OUTPUT_DIRECTORY_${upper}, which the CEF file copy does not "
                "support on Linux: set RUNTIME_OUTPUT_DIRECTORY instead")
        endif()
    endforeach()
    get_target_property(binary_dir ${target_name} BINARY_DIR)
    get_target_property(directory ${target_name} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT directory)
        set(directory "${binary_dir}")
    elseif(NOT IS_ABSOLUTE "${directory}" AND NOT directory MATCHES "^\\$<")
        set(directory "${binary_dir}/${directory}")
    endif()
    get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    # Multi-config generators add the configuration unless the directory is a generator expression.
    if(multi AND NOT directory MATCHES "\\$<")
        set(directory "${directory}/$<CONFIG>")
    endif()
    set(${out_dir} "${directory}" PARENT_SCOPE)
endfunction()

# The output directory of a target, checked again once its directory is processed.
function(_cef_planned_output_directory target_name out_dir)
    _cef_output_directory(${target_name} directory)
    cmake_language(EVAL CODE
        "cmake_language(DEFER CALL _cef_check_output_directory [==[${target_name}]==] [==[${directory}]==])")
    set(${out_dir} "${directory}" PARENT_SCOPE)
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
        set(runtime_files ${CEF_WINDOWS_RUNTIME_FILES})
        foreach(file_name IN LISTS CEF_WINDOWS_OPTIONAL_RUNTIME_FILES)
            if(EXISTS "${cef_build_dir}/${file_name}")
                list(APPEND runtime_files "${file_name}")
            endif()
        endforeach()
        _cef_copy_files_to_target(${target_name}
            "${cef_build_dir}"
            "${target_dir}"
            "${runtime_files}"
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
        _cef_planned_output_directory(${target_name} output)
        _cef_shared_copy(${target_name} "${output}" NAME runtime SOURCE_DIR "${cef_build_dir}" FILES ${CEF_LINUX_RUNTIME_FILES})
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
        
        # Copy to application directory (standard deployment), once per output directory
        _cef_planned_output_directory(${target_name} output)
        _cef_shared_copy(${target_name} "${output}" NAME resources SOURCE_DIR "${CEF_RESOURCE_SOURCE_DIR}"
            FILES ${CEF_RESOURCE_FILES} DIRECTORY locales)

        # Copy to CEF binary directory (ICU loading fix), once for all targets. As before, this is the Release
        # directory in every configuration.
        _cef_shared_copy(${target_name} "${CEF_BINARY_RELEASE_DIR}" NAME resources SOURCE_DIR "${CEF_RESOURCE_SOURCE_DIR}"
            FILES ${CEF_RESOURCE_FILES} DIRECTORY locales)
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