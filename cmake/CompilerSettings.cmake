# CompilerSettings.cmake
# Handle compiler-specific configuration and generator optimizations

# Configure Ninja generator for optimal builds
if(CMAKE_GENERATOR STREQUAL "Ninja")
  # Enable colored output for Ninja
  set(CMAKE_COLOR_MAKEFILE ON)
  set(CMAKE_COLOR_DIAGNOSTICS ON)
  # Use response files for long command lines to handle very long command lines
  set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
  message(STATUS "Using Ninja build system with optimizations")
endif()

# MSVC specific compiler settings
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
