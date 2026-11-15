# The real packages of the local npm registry of the tests (test/web/registry-packages.json): downloaded at configure time
# like the other dependencies, verified against their SHA-512 integrity, never committed. A wf script then builds the
# registry's files from them (test/web/registry/), so that no test reaches the public registry and none needs a JavaScript runtime.
include(WebFrontBootstrap)

# A directory holding the listed archives (<name>-<version>.tgz, @scope/name as scope-name-<version>.tgz) replaces the
# downloads on a machine without network access.
set(WEBFRONT_REGISTRY_DIR "" CACHE PATH "Directory with the pinned archives of the test registry, instead of downloading them")

# Fills <directory> with the archives of <list>, each named <file name of its URL> and verified; sets <output> to the
# directory. A wrong digest or a missing archive stops the configure.
function(webfront_provision_registry output list directory)
    file(READ "${list}" content)
    string(JSON count LENGTH "${content}" packages)
    math(EXPR last "${count} - 1")
    file(MAKE_DIRECTORY "${directory}")
    foreach(index RANGE 0 ${last})
        string(JSON url GET "${content}" packages ${index} url)
        string(JSON integrity GET "${content}" packages ${index} integrity)
        webfront_sri_to_sha512("${integrity}" expected)
        get_filename_component(name "${url}" NAME)
        # Scoped packages share base names with others of the same version: the file name carries the scope.
        string(JSON package GET "${content}" packages ${index} name)
        if(package MATCHES "^@([^/]+)/")
            set(name "${CMAKE_MATCH_1}-${name}")
        endif()
        set(path "${directory}/${name}")
        if(WEBFRONT_REGISTRY_DIR)
            if(NOT EXISTS "${WEBFRONT_REGISTRY_DIR}/${name}")
                message(FATAL_ERROR "Test registry archive ${WEBFRONT_REGISTRY_DIR}/${name} is missing: WEBFRONT_REGISTRY_DIR must hold it")
            endif()
            file(COPY_FILE "${WEBFRONT_REGISTRY_DIR}/${name}" "${path}" ONLY_IF_DIFFERENT)
        else()
            webfront_download_verified("${path}" URL "${url}" SHA512 "${expected}"
                FAILURE "Cannot download ${url}. Provision the archives with -DWEBFRONT_REGISTRY_DIR=<directory holding ${name}>"
                MISMATCH "Integrity check failed for ${name}: expected ${integrity}")
        endif()
        file(SHA512 "${path}" actual)
        if(NOT actual STREQUAL expected)
            file(REMOVE "${path}")
            message(FATAL_ERROR "Integrity check failed for ${name}: expected ${integrity}")
        endif()
    endforeach()
    set(${output} "${directory}" PARENT_SCOPE)
endfunction()
