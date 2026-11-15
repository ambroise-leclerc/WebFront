# Bootstrap provisioning at configure time: SRI conversion, verified download, machine download cache, retries, local
# directory, integrity failures.
# Usage: cmake -DROOT=<source> -DWORK=<scratch directory> -P CheckBootstrapProvisioning.cmake
include("${ROOT}/cmake/WebFrontBootstrap.cmake")

# No line break: a text-mode write would turn it into CRLF on Windows and change the digest.
set(content "webfront bootstrap test archive")
set(sri "sha512-tp5zEsnN4U2b4SrnPY7GY6cYRK/sFQRO9ZX/xlR7mRyMxqedrks3Oc6EVPRYECyrQE8C4FAg6wH5CVNFoNoT6Q==")
set(hex "b69e7312c9cde14d9be12ae73d8ec663a71844afec15044ef595ffc6547b991c8cc6a79dae4b3739ce8454f458102cab404f02e05020eb01f9095345a0da13e9")

function(expect_equal actual expected message)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${message}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

# The pinned value of the repository is a well-formed SHA-512.
webfront_sri_to_sha512("${WEBFRONT_BOOTSTRAP_INTEGRITY}" pinned)
string(LENGTH "${pinned}" pinnedLength)
expect_equal("${pinnedLength}" 128 "The pinned integrity must decode to 64 bytes")

webfront_sri_to_sha512("${sri}" decoded)
expect_equal("${decoded}" "${hex}" "SRI conversion")

file(REMOVE_RECURSE "${WORK}")
# The SRI of a file built by a test round-trips through the conversion above.
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/sri.bin" "${content}")
webfront_file_sri("${WORK}/sri.bin" encoded)
expect_equal("${encoded}" "${sri}" "SRI encoding")

# Each case runs in its own cmake process: a failed integrity check is a FATAL_ERROR.
file(MAKE_DIRECTORY "${WORK}/source" "${WORK}/local")
file(WRITE "${WORK}/source/environment.tgz" "${content}")
file(WRITE "${WORK}/local/environment.tgz" "${content}")
file(WRITE "${WORK}/tampered.tgz" "tampered\n")
file(WRITE "${WORK}/run.cmake" [[
include("${ROOT}/cmake/WebFrontBootstrap.cmake")
if(NOT DESTINATION)
    set(DESTINATION "${WORK}/cache")
endif()
if(LOCAL)
    webfront_provision_bootstrap(archive URL "${URL}" INTEGRITY "${INTEGRITY}" DESTINATION "${DESTINATION}" LOCAL_DIR "${LOCAL}")
else()
    webfront_provision_bootstrap(archive URL "${URL}" INTEGRITY "${INTEGRITY}" DESTINATION "${DESTINATION}")
endif()
message(STATUS "provisioned ${archive}")
]])

function(provision name result_var output_var)
    cmake_parse_arguments(ARG "" "URL;INTEGRITY;LOCAL;DESTINATION;CACHE" "" ${ARGN})
    # No retry delay, and the machine cache only where a case names one.
    execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${ROOT} -DWORK=${WORK} -DURL=${ARG_URL} -DINTEGRITY=${ARG_INTEGRITY}
            -DLOCAL=${ARG_LOCAL} -DDESTINATION=${ARG_DESTINATION} -DWEBFRONT_DOWNLOAD_CACHE=${ARG_CACHE}
            -DWEBFRONT_DOWNLOAD_RETRY_DELAY=0 -P "${WORK}/run.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${result_var} "${result}" PARENT_SCOPE)
    # CMake wraps a long message wherever its paths make it long: the checks read it on one line.
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out}${err}")
    set(${output_var} "${text}" PARENT_SCOPE)
endfunction()

# A file URL: a Windows drive letter needs a third slash.
if(WORK MATCHES "^[A-Za-z]:")
    set(scheme "file:///")
else()
    set(scheme "file://")
endif()
set(url "${scheme}${WORK}/source/environment.tgz")

provision(download result text URL "${url}" INTEGRITY "${sri}")
expect_equal("${result}" 0 "A matching download must be provisioned: ${text}")
if(NOT EXISTS "${WORK}/cache/environment.tgz")
    message(FATAL_ERROR "The verified archive must be kept in the destination")
endif()

# A cached archive is reused without the source.
file(REMOVE "${WORK}/source/environment.tgz")
provision(reuse result text URL "${url}" INTEGRITY "${sri}")
expect_equal("${result}" 0 "A verified cached archive must be reused: ${text}")

# A wrong digest stops the configure and leaves no unverified archive behind.
file(REMOVE_RECURSE "${WORK}/cache")
file(COPY "${WORK}/tampered.tgz" DESTINATION "${WORK}/source")
file(RENAME "${WORK}/source/tampered.tgz" "${WORK}/source/environment.tgz")
provision(wrong result text URL "${url}" INTEGRITY "${sri}")
if(result EQUAL 0 OR NOT text MATCHES "integrity check failed")
    message(FATAL_ERROR "A wrong integrity must fail the configure: ${result} ${text}")
endif()
if(EXISTS "${WORK}/cache/environment.tgz")
    message(FATAL_ERROR "A downloaded archive that fails its integrity check must be removed")
endif()

# A download failure is tried again, then names the offline alternative.
provision(missing result text URL "${scheme}${WORK}/absent/environment.tgz" INTEGRITY "${sri}")
if(result EQUAL 0 OR NOT text MATCHES "WEBFRONT_BOOTSTRAP_DIR" OR NOT text MATCHES "3 attempts" OR NOT text MATCHES "trying again")
    message(FATAL_ERROR "A failed download must be tried again, then point to WEBFRONT_BOOTSTRAP_DIR: ${result} ${text}")
endif()

# The machine download cache: a download fills it, and another build tree then gets the archive from it alone.
file(WRITE "${WORK}/source/environment.tgz" "${content}")
set(machine "${WORK}/machine")
string(SUBSTRING "${hex}" 0 32 key)
provision(cacheFill result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-a" CACHE "${machine}")
expect_equal("${result}" 0 "A download must fill the machine cache: ${text}")
foreach(file "${machine}/${key}/environment.tgz" "${WORK}/tree-a/environment.tgz")
    file(SHA512 "${file}" digest)
    expect_equal("${digest}" "${hex}" "The cached and provisioned archives must be the verified one (${file})")
endforeach()
file(GLOB partials "${machine}/${key}/*.partial")
expect_equal("${partials}" "" "No partial download may stay in the cache")
file(REMOVE "${WORK}/source/environment.tgz")
provision(cacheHit result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-b" CACHE "${machine}")
expect_equal("${result}" 0 "A new build tree must be provisioned from the machine cache without the source: ${text}")
file(SHA512 "${WORK}/tree-b/environment.tgz" digest)
expect_equal("${digest}" "${hex}" "The archive linked from the cache")
# A damaged cache entry is downloaded again, never used.
file(REMOVE "${machine}/${key}/environment.tgz")
file(WRITE "${machine}/${key}/environment.tgz" "damaged")
file(WRITE "${WORK}/source/environment.tgz" "${content}")
provision(cacheRepair result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-c" CACHE "${machine}")
expect_equal("${result}" 0 "A damaged cache entry must be downloaded again: ${text}")
foreach(file "${machine}/${key}/environment.tgz" "${WORK}/tree-c/environment.tgz")
    file(SHA512 "${file}" digest)
    expect_equal("${digest}" "${hex}" "The repaired archive (${file})")
endforeach()
# Data with another digest stops the configure and stays out of the cache.
file(WRITE "${WORK}/source/environment.tgz" "tampered")
file(REMOVE_RECURSE "${machine}")
provision(cacheWrong result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-d" CACHE "${machine}")
if(result EQUAL 0 OR NOT text MATCHES "integrity check failed" OR EXISTS "${machine}/${key}/environment.tgz" OR EXISTS "${WORK}/tree-d/environment.tgz")
    message(FATAL_ERROR "A wrong digest must fail and leave nothing in the cache or the build tree: ${result} ${text}")
endif()

# A local provisioning directory replaces the download and is verified too.
provision(local result text URL "https://offline.invalid/environment.tgz" INTEGRITY "${sri}" LOCAL "${WORK}/local")
expect_equal("${result}" 0 "A local directory must replace the download: ${text}")
file(WRITE "${WORK}/local/environment.tgz" "tampered\n")
provision(localWrong result text URL "https://offline.invalid/environment.tgz" INTEGRITY "${sri}" LOCAL "${WORK}/local")
if(result EQUAL 0 OR NOT text MATCHES "integrity check failed")
    message(FATAL_ERROR "A tampered local archive must fail the configure: ${result} ${text}")
endif()
file(REMOVE "${WORK}/local/environment.tgz")
provision(localMissing result text URL "https://offline.invalid/environment.tgz" INTEGRITY "${sri}" LOCAL "${WORK}/local")
if(result EQUAL 0 OR NOT text MATCHES "is missing")
    message(FATAL_ERROR "An absent local archive must fail the configure: ${result} ${text}")
endif()

# A cache that exists but refuses files (a shared, read-only one): a verified entry still serves, and a download falls back
# to the build tree with a warning instead of stopping the configure. Only POSIX permissions can make it so, and a process
# that writes through them (root) cannot test it.
if(NOT CMAKE_HOST_WIN32)
    file(WRITE "${WORK}/source/environment.tgz" "${content}")
    set(readonly "${WORK}/readonly")
    file(REMOVE_RECURSE "${readonly}")
    file(MAKE_DIRECTORY "${readonly}/${key}")
    file(CHMOD "${readonly}" "${readonly}/${key}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E touch "${readonly}/${key}/probe" RESULT_VARIABLE writable OUTPUT_QUIET ERROR_QUIET)
    if(writable EQUAL 0)
        file(REMOVE "${readonly}/${key}/probe")
        message(STATUS "This process writes into read-only directories: the read-only cache cases are skipped")
    else()
        provision(readonlyEmpty result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-e" CACHE "${readonly}")
        expect_equal("${result}" 0 "A read-only cache must not stop the configure: ${text}")
        if(NOT text MATCHES "cannot be written")
            message(FATAL_ERROR "A read-only cache must be reported: ${text}")
        endif()
        file(SHA512 "${WORK}/tree-e/environment.tgz" digest)
        expect_equal("${digest}" "${hex}" "The archive kept in the build tree beside a read-only cache")
        file(CHMOD "${readonly}/${key}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        file(COPY_FILE "${WORK}/source/environment.tgz" "${readonly}/${key}/environment.tgz")
        file(CHMOD "${readonly}/${key}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        file(REMOVE "${WORK}/source/environment.tgz")
        provision(readonlyEntry result text URL "${url}" INTEGRITY "${sri}" DESTINATION "${WORK}/tree-f" CACHE "${readonly}")
        expect_equal("${result}" 0 "A verified entry of a read-only cache must serve without the source: ${text}")
        file(SHA512 "${WORK}/tree-f/environment.tgz" digest)
        expect_equal("${digest}" "${hex}" "The archive linked from a read-only cache")
    endif()
    file(CHMOD "${readonly}" "${readonly}/${key}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()

# The test registry keeps the verified archives already in the build tree: its offline directory need not stay mounted.
file(MAKE_DIRECTORY "${WORK}/offline")
file(WRITE "${WORK}/offline/environment.tgz" "${content}")
file(WRITE "${WORK}/packages.json" "{\"packages\": [{\"name\": \"environment\", \"url\": \"https://offline.invalid/environment.tgz\", \"integrity\": \"${sri}\"}]}")
file(WRITE "${WORK}/registry.cmake" [[
list(APPEND CMAKE_MODULE_PATH "${ROOT}/cmake")
include("${ROOT}/cmake/WebFrontRegistry.cmake")
webfront_provision_registry(directory "${WORK}/packages.json" "${WORK}/registry-tree")
message(STATUS "provisioned ${directory}")
]])
foreach(round mounted unmounted)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${ROOT} -DWORK=${WORK} -DWEBFRONT_REGISTRY_DIR=${WORK}/offline
            -DWEBFRONT_DOWNLOAD_CACHE= -P "${WORK}/registry.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    expect_equal("${result}" 0 "The ${round} offline registry directory must provision the build tree: ${out}${err}")
    file(REMOVE_RECURSE "${WORK}/offline")
endforeach()

# A malformed integrity is refused before anything is downloaded.
provision(malformed result text URL "${url}" INTEGRITY "sha256-abc")
if(result EQUAL 0 OR NOT text MATCHES "sha512- SRI")
    message(FATAL_ERROR "A non SHA-512 integrity must be refused: ${result} ${text}")
endif()
message(STATUS "Bootstrap provisioning checks passed")
