# GCC 16 crashes inside stdexec (an internal compiler error in __query.hpp, reproduced by including
# <stdexec/execution.hpp> alone under -fmodules-ts) when CMake adds -fmodules-ts, which it does for every
# C++20+ target while module scanning is not OFF (CMP0155). WebFront consumers include headers only, so
# module scanning is never needed for the targets that link WebFront::WebFront.
# Limit: source-level CXX_SCAN_FOR_MODULES and CXX_MODULES file sets still make CMake scan a target, and
# neither the check nor webfront_disable_module_scan() covers them.

# Turn module scanning off for the given targets.
function(webfront_disable_module_scan)
    foreach(_target IN LISTS ARGN)
        set_target_properties(${_target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endforeach()
endfunction()

function(_webfront_collect_scanned_targets directory out)
    set(_found "")
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _target STREQUAL "WebFront")
            continue()
        endif()
        get_target_property(_links ${_target} LINK_LIBRARIES)
        if(NOT _links OR NOT ("WebFront::WebFront" IN_LIST _links OR "WebFront" IN_LIST _links))
            continue()
        endif()
        # CMake initializes the property from CMAKE_CXX_SCAN_FOR_MODULES when the target is created, so a
        # later change of the variable has no effect on it. An unset property means the variable was unset
        # then, and scanning follows CMP0155 (on for C++20 and later).
        get_target_property(_scan ${_target} CXX_SCAN_FOR_MODULES)
        if(_scan STREQUAL "_scan-NOTFOUND")
            set(_scan ON)
        endif()
        if(_scan)
            list(APPEND _found ${_target})
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _webfront_collect_scanned_targets("${_sub}" _sub_found)
        list(APPEND _found ${_sub_found})
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# Runs when the consuming project has finished configuring, so the final scan settings are known.
function(_webfront_report_module_scan)
    _webfront_collect_scanned_targets("${CMAKE_SOURCE_DIR}" _targets)
    if(_targets)
        list(JOIN _targets ", " _names)
        message(WARNING
            "GCC ${CMAKE_CXX_COMPILER_VERSION} crashes in stdexec (internal compiler error) when CMake "
            "scans C++ modules (-fmodules-ts); affected targets linking WebFront::WebFront: ${_names}. "
            "Add set(CMAKE_CXX_SCAN_FOR_MODULES OFF) before your targets, or call "
            "webfront_disable_module_scan(<target>...).")
    endif()
endfunction()

# Warn, at the end of the consuming project's configuration, about targets exposed to the crash.
function(webfront_check_module_scan)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16")
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _webfront_report_module_scan)
    endif()
endfunction()
