# The bundles that are checked in (the bootstrap loader, the packages of the environment) are built from the readable sources
# of js/developer/environment and the files listed in js/developer/tools/bundle-inputs.json. bundles.json records the SHA-256 of
# each of those files and of the bundles themselves: a source changed without rebuilding the bundles is detected here, with no
# browser. wf rebuilds the bundles and the checksums (`wf run build build-distribution --refresh-bootstrap`), and the
# SelfBuildRefresh cases prove that the checked-in ones are what wf produces.
#   cmake -DROOT=<checkout> -P test/cmake/CheckBundleInputs.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()
set(manifest_path "${ROOT}/js/developer/environment/bundles.json")
set(list_path "${ROOT}/js/developer/tools/bundle-inputs.json")
file(READ "${manifest_path}" manifest)
file(READ "${list_path}" listed)

# Every input of the build has a checksum, and only those: the two files agree.
string(JSON count LENGTH "${listed}")
math(EXPR last "${count} - 1")
set(inputs "")
foreach(index RANGE 0 ${last})
    string(JSON path GET "${listed}" ${index})
    list(APPEND inputs "${path}")
endforeach()
string(JSON recorded_count LENGTH "${manifest}")
if(NOT recorded_count EQUAL count)
    message(FATAL_ERROR "bundles.json records ${recorded_count} checksums for ${count} inputs: "
        "run wf run build build-distribution --refresh-bootstrap")
endif()

set(changed "")
foreach(path IN LISTS inputs)
    string(JSON expected ERROR_VARIABLE missing GET "${manifest}" "${path}")
    if(missing)
        list(APPEND changed "${path} (no checksum)")
        continue()
    endif()
    if(NOT EXISTS "${ROOT}/${path}")
        list(APPEND changed "${path} (missing)")
        continue()
    endif()
    file(SHA256 "${ROOT}/${path}" actual)
    if(NOT actual STREQUAL expected)
        list(APPEND changed "${path}")
    endif()
endforeach()
if(changed)
    list(JOIN changed "\n  " text)
    message(FATAL_ERROR "The inputs of the bundles changed without rebuilding them:\n  ${text}\n"
        "Run wf run build build-distribution --refresh-bootstrap")
endif()
message(STATUS "${count} bundle inputs match bundles.json")
