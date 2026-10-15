# Builds the local npm registry that webtest/js/wf-specs.mjs installs from, so the browser test needs
# no public network: tarballs laid out like npm's (files under package/), packuments carrying their
# SHA-512 integrity, and a search result. Served under /registry/ by RegistryFS in JasmineTest.cpp.
#
#   cmake -DOUTPUT_DIR=<dir> -P MakeToolRegistry.cmake

cmake_minimum_required(VERSION 3.31)

if(NOT OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

# CMake has no base64, which Subresource Integrity strings use: convert the hex digest, 3 bytes at a time.
function(base64_from_hex hex out)
    set(alphabet "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
    string(LENGTH "${hex}" length)
    set(result "")
    set(index 0)
    while(index LESS length)
        string(SUBSTRING "${hex}" ${index} 6 group)
        string(LENGTH "${group}" taken)
        set(padded "${group}000000")
        string(SUBSTRING "${padded}" 0 6 padded)
        math(EXPR value "0x${padded}")
        set(encoded "")
        foreach(shift 18 12 6 0)
            math(EXPR digit "(${value} >> ${shift}) & 63")
            string(SUBSTRING "${alphabet}" ${digit} 1 character)
            string(APPEND encoded "${character}")
        endforeach()
        if(taken EQUAL 2)
            string(SUBSTRING "${encoded}" 0 2 encoded)
            string(APPEND encoded "==")
        elseif(taken EQUAL 4)
            string(SUBSTRING "${encoded}" 0 3 encoded)
            string(APPEND encoded "=")
        endif()
        string(APPEND result "${encoded}")
        math(EXPR index "${index} + 6")
    endwhile()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

set(work "${OUTPUT_DIR}/.work")
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/tarballs" "${OUTPUT_DIR}/-/v1")

# Packs ${work}/<file name>/package into tarballs/<file name>.tgz and sets <out> to its integrity.
function(pack file_name out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar czf "${OUTPUT_DIR}/tarballs/${file_name}.tgz" --format=pax package
        WORKING_DIRECTORY "${work}/${file_name}"
        RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "Cannot pack ${file_name}")
    endif()
    file(SHA512 "${OUTPUT_DIR}/tarballs/${file_name}.tgz" digest)
    base64_from_hex("${digest}" encoded)
    set(${out} "sha512-${encoded}" PARENT_SCOPE)
endfunction()

# Writes <name>.json, the packument of one version tagged latest.
function(write_packument name version manifest_fields tarball integrity)
    file(WRITE "${OUTPUT_DIR}/${name}.json" "{
  \"name\": \"${name}\",
  \"dist-tags\": {\"latest\": \"${version}\"},
  \"versions\": {
    \"${version}\": {
      \"name\": \"${name}\",
      \"version\": \"${version}\",
      ${manifest_fields},
      \"dist\": {\"tarball\": \"/registry/tarballs/${tarball}.tgz\", \"integrity\": \"${integrity}\"}
    }
  }
}
")
endfunction()

# webfront-hello: an entry page in a subdirectory, a file larger than one upload chunk (256 KiB), an
# empty file, a path split into the ustar prefix and name fields, and a name longer than both, which
# needs a pax header.
set(hello_fields "\"description\": \"Says hello\", \"keywords\": [\"webfront-tool\"], \"webfront\": {\"kind\": \"tool\", \"title\": \"Hello tool\", \"entry\": \"dist/index.html\"}")
set(hello "${work}/webfront-hello-1.0.0/package")
file(WRITE "${hello}/package.json" "{\"name\": \"webfront-hello\", \"version\": \"1.0.0\", ${hello_fields}}\n")
file(WRITE "${hello}/dist/index.html" "<!DOCTYPE html><title>Hello tool</title><p>Hello from an installed tool</p>\n")
string(REPEAT "0123456789" 30000 big)
file(WRITE "${hello}/dist/big.txt" "${big}")
file(WRITE "${hello}/empty.txt" "")
string(REPEAT "d" 60 directory)
string(REPEAT "f" 60 name)
file(WRITE "${hello}/deep/${directory}/${name}.txt" "long path")
string(REPEAT "n" 120 long_name)
file(WRITE "${hello}/${long_name}.txt" "long name")
pack(webfront-hello-1.0.0 hello_integrity)
write_packument(webfront-hello 1.0.0 "${hello_fields}" webfront-hello-1.0.0 "${hello_integrity}")

# @webfront-test/plot: a scoped name, and an entry page left to its index.html default.
set(plot_fields "\"description\": \"Plots values\", \"keywords\": [\"webfront-tool\"], \"webfront\": {\"kind\": \"tool\", \"title\": \"Plot\"}")
set(plot "${work}/webfront-test-plot-1.0.0/package")
file(WRITE "${plot}/package.json" "{\"name\": \"@webfront-test/plot\", \"version\": \"1.0.0\", ${plot_fields}}\n")
file(WRITE "${plot}/index.html" "<!DOCTYPE html><title>Plot</title><p>Plot</p>\n")
pack(webfront-test-plot-1.0.0 plot_integrity)
write_packument(@webfront-test/plot 1.0.0 "${plot_fields}" webfront-test-plot-1.0.0 "${plot_integrity}")

# Failures: a tarball whose hash does not match, and a package that is not a WebFront tool.
set(badhash_fields "\"keywords\": [\"webfront-tool\"], \"webfront\": {\"kind\": \"tool\", \"title\": \"Bad hash\"}")
write_packument(webfront-badhash 1.0.0 "${badhash_fields}" webfront-hello-1.0.0 "${plot_integrity}")
write_packument(webfront-notool 1.0.0 "\"description\": \"A library\"" webfront-hello-1.0.0 "${hello_integrity}")

# The registry filters by keywords and term; this fixture returns both tools whatever the query.
file(WRITE "${OUTPUT_DIR}/-/v1/search.json" "{\"objects\": [
  {\"package\": {\"name\": \"webfront-hello\", \"version\": \"1.0.0\", \"description\": \"Says hello\"}},
  {\"package\": {\"name\": \"@webfront-test/plot\", \"version\": \"1.0.0\", \"description\": \"Plots values\"}}
]}
")

file(REMOVE_RECURSE "${work}")
