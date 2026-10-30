# GCC 16 crashes inside stdexec (an internal compiler error in __query.hpp, reduced to an extern const
# variable redeclared inline constexpr under -fmodules-ts) when CMake adds -fmodules-ts, which it does
# for every C++20+ target while module scanning is not OFF (CMP0155). WebFront consumers include headers
# only, so module scanning is never needed for the targets that link WebFront::WebFront.
# Limits: source-level CXX_SCAN_FOR_MODULES and CXX_MODULES file sets still make CMake scan a target, and
# neither the check nor webfront_disable_module_scan() covers them. The check only sees targets that link
# WebFront::WebFront directly, not those reaching it through another library of the project.

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
            # Scanning by default only applies when the embedding project has CMP0155 NEW.
            if(WEBFRONT_PARENT_CMP0155 STREQUAL "OLD")
                set(_scan OFF)
            else()
                set(_scan ON)
            endif()
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
# CMAKE_SOURCE_DIR is the root of the embedding project, whose targets are inspected.
function(_webfront_report_module_scan)
    _webfront_collect_scanned_targets("${CMAKE_SOURCE_DIR}" _targets)
    if(_targets)
        list(JOIN _targets ", " _names)
        message(WARNING
            "GCC ${CMAKE_CXX_COMPILER_VERSION} crashes in stdexec (internal compiler error) when CMake "
            "scans C++ modules (-fmodules-ts); affected targets linking WebFront::WebFront: ${_names}. "
            "Call webfront_disable_module_scan(<target>...) for the targets that use no C++ modules, or "
            "add set(CMAKE_CXX_SCAN_FOR_MODULES OFF) before creating your targets if none of them does.")
    endif()
endfunction()

# Warn, at the end of the consuming project's configuration, about targets exposed to the crash.
# WEBFRONT_MODULE_SCAN_CHECK_FORCE runs the check with any compiler; it is a hook of the regression test.
function(webfront_check_module_scan)
    if(WEBFRONT_MODULE_SCAN_CHECK_FORCE
        OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16"))
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _webfront_report_module_scan)
    endif()
endfunction()
