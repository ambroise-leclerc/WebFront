# Configure Ninja generator for optimal builds
if(CMAKE_GENERATOR STREQUAL "Ninja")
  # Enable colored output for Ninja
  set(CMAKE_COLOR_MAKEFILE ON)
  set(CMAKE_COLOR_DIAGNOSTICS ON)
  # Use response files for long command lines to handle very long command lines
  set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
  message(STATUS "Using Ninja build system with optimizations")
endif()

# Set a default build type if none was specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui, ccmake
  set_property(
    CACHE CMAKE_BUILD_TYPE
    PROPERTY STRINGS
             "Debug"
             "Release"
             "MinSizeRel"
             "RelWithDebInfo")
endif()

# Generate compile_commands.json to make it easier to work with clang based tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "Generate compile_commands.json for clang based tools")

option(ENABLE_IPO "Enable Interprocedural Optimization, aka Link Time Optimization (LTO)" OFF)

if(ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(
    RESULT
    result
    OUTPUT
    output)
  if(result)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(SEND_ERROR "IPO is not supported: ${output}")
  endif()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  add_compile_options(-fcolor-diagnostics)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-fdiagnostics-color=always)
else()
  message(STATUS "No colored compiler diagnostic set for '${CMAKE_CXX_COMPILER_ID}' compiler.")
endif()

# MSVC specific compiler settings function
function(configure_msvc_settings target_name)
  if(MSVC)
    target_compile_options(${target_name} INTERFACE 
      /EHsc  # Enable exception handling
      /W4    # Warning level 4
      /wd4530  # Disable C4530 warning about exception handling
    )
    message(STATUS "Applied MSVC-specific compiler settings to ${target_name}")
  endif()
endfunction()

# Function to apply compiler-specific settings to a target
function(configure_compiler_settings target_name)
  # Apply MSVC settings if applicable
  configure_msvc_settings(${target_name})
  
  # Add other compiler-specific configurations here as needed
  # For example, GCC or Clang specific settings could be added here
endfunction()

