# Bootstrap provisioning at configure time: SRI conversion, verified download, local directory, integrity failures.
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
if(LOCAL)
    webfront_provision_bootstrap(archive URL "${URL}" INTEGRITY "${INTEGRITY}" DESTINATION "${WORK}/cache" LOCAL_DIR "${LOCAL}")
else()
    webfront_provision_bootstrap(archive URL "${URL}" INTEGRITY "${INTEGRITY}" DESTINATION "${WORK}/cache")
endif()
message(STATUS "provisioned ${archive}")
]])

function(provision name result_var output_var)
    cmake_parse_arguments(ARG "" "URL;INTEGRITY;LOCAL" "" ${ARGN})
    execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${ROOT} -DWORK=${WORK} -DURL=${ARG_URL} -DINTEGRITY=${ARG_INTEGRITY}
            -DLOCAL=${ARG_LOCAL} -P "${WORK}/run.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${output_var} "${out}${err}" PARENT_SCOPE)
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

# A download failure names the offline alternative.
provision(missing result text URL "${scheme}${WORK}/absent/environment.tgz" INTEGRITY "${sri}")
if(result EQUAL 0 OR NOT text MATCHES "WEBFRONT_BOOTSTRAP_DIR")
    message(FATAL_ERROR "A failed download must point to WEBFRONT_BOOTSTRAP_DIR: ${result} ${text}")
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

# A malformed integrity is refused before anything is downloaded.
provision(malformed result text URL "${url}" INTEGRITY "sha256-abc")
if(result EQUAL 0 OR NOT text MATCHES "sha512- SRI")
    message(FATAL_ERROR "A non SHA-512 integrity must be refused: ${result} ${text}")
endif()
message(STATUS "Bootstrap provisioning checks passed")
