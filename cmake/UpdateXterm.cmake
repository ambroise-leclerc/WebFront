# Maintenance script, not part of the build: refreshes the vendored xterm.js files in
# third_party/xterm from the npm registry (network access required), then regenerate the embedded
# headers with the webfront-embedded-assets target and review the diff.
#
#   cmake -DXTERM_VERSION=6.0.0 -DFIT_VERSION=0.11.0 -P cmake/UpdateXterm.cmake

cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED XTERM_VERSION OR NOT DEFINED FIT_VERSION)
    message(FATAL_ERROR "Set XTERM_VERSION and FIT_VERSION, e.g. -DXTERM_VERSION=6.0.0 -DFIT_VERSION=0.11.0")
endif()

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(destination "${root}/third_party/xterm")
string(RANDOM LENGTH 8 suffix)
set(work "${CMAKE_CURRENT_BINARY_DIR}/webfront-xterm-update-${suffix}")

function(_fetch_package url archive)
    file(DOWNLOAD "${url}" "${archive}" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Download of ${url} failed: ${status}")
    endif()
endfunction()

_fetch_package("https://registry.npmjs.org/@xterm/xterm/-/xterm-${XTERM_VERSION}.tgz" "${work}/xterm.tgz")
_fetch_package("https://registry.npmjs.org/@xterm/addon-fit/-/addon-fit-${FIT_VERSION}.tgz" "${work}/fit.tgz")
file(ARCHIVE_EXTRACT INPUT "${work}/xterm.tgz" DESTINATION "${work}/xterm")
file(ARCHIVE_EXTRACT INPUT "${work}/fit.tgz" DESTINATION "${work}/fit")

file(COPY_FILE "${work}/xterm/package/lib/xterm.mjs" "${destination}/xterm.mjs")
file(COPY_FILE "${work}/xterm/package/css/xterm.css" "${destination}/xterm.css")
file(COPY_FILE "${work}/xterm/package/LICENSE" "${destination}/LICENSE")
file(COPY_FILE "${work}/fit/package/lib/addon-fit.mjs" "${destination}/addon-fit.mjs")
file(COPY_FILE "${work}/fit/package/LICENSE" "${destination}/addon-fit-LICENSE")
file(WRITE "${destination}/VERSION" "@xterm/xterm ${XTERM_VERSION}\n@xterm/addon-fit ${FIT_VERSION}\n")
file(REMOVE_RECURSE "${work}")

message(STATUS "Updated ${destination}; now run: cmake --build <build> --target webfront-embedded-assets")
