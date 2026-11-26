# webfront_embed_directory() without a compiler (#675): the list of files it embeds (sorted, EXCLUDE applied), the header
# its renderer writes (routes, sizes and words), and the configuration errors it reports for a wrong name, class,
# namespace, directory or target.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch> -P CheckEmbedDirectoryArguments.cmake
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

# Configures a project with no language, whose target `app` is an INTERFACE library (also reachable as other::app), with
# `body` after it.
function(configure_case name body out_result out_output)
    set(project "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${project}")
    file(WRITE "${project}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.31)
project(EmbedDirectoryArguments LANGUAGES NONE)
# Loaded in a scope of its own, as add_subdirectory(WebFront) does: the function must not need a variable of that scope.
function(load_webfront)
    include(\"${ROOT}/cmake/WebFrontEmbedDirectory.cmake\")
endfunction()
load_webfront()
add_library(app INTERFACE)
add_library(other::app ALIAS app)
${body}
")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${project}" -B "${project}/build"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${out_result} "${result}" PARENT_SCOPE)
    set(${out_output} "${output}${error}" PARENT_SCOPE)
endfunction()

function(expect_failure name body pattern)
    configure_case("${name}" "${body}" result output)
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: the configuration must fail")
    endif()
    string(REGEX REPLACE "[ \n]+" " " flat "${output}")
    if(NOT flat MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: expected '${pattern}' in:\n${output}")
    endif()
    message(STATUS "${name}: refused as expected")
endfunction()

set(ui "${WORK_DIR}/ui")
file(MAKE_DIRECTORY "${ui}/sub/deeper" "${ui}/.git")
file(WRITE "${ui}/index.html" "<!doctype html>\n")
file(WRITE "${ui}/sub/hello.js" "hello")
file(WRITE "${ui}/sub/deeper/forty-one.txt" "01234567890123456789012345678901234567890")
file(WRITE "${ui}/empty.css" "")
file(WRITE "${ui}/.DS_Store" "x")
file(WRITE "${ui}/.git/HEAD" "x")
file(WRITE "${ui}/notes.md" "skipped by its extension")
file(WRITE "${ui}/@a@ b+~.svg" "<svg/>")

# The accepted case: an alias target, EXCLUDE, the default class and namespace.
configure_case(accepted "webfront_embed_directory(other::app \"${ui}/\" EXCLUDE \"(^|/)[.]\" \"[.]md$\")
get_target_property(includes app INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS \"includes=\${includes}\")
get_target_property(dependencies app MANUALLY_ADDED_DEPENDENCIES)
message(STATUS \"dependencies=\${dependencies}\")" result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "accepted: the configuration failed:\n${output}")
endif()
set(build "${WORK_DIR}/accepted/build")
if(NOT output MATCHES "includes=${build}/webfront_embed/app\n" OR NOT output MATCHES "dependencies=webfront_embed_app_EmbeddedDirectory\n")
    message(FATAL_ERROR "accepted: the target must include the generated directory and depend on its generation:\n${output}")
endif()
set(manifest "${build}/webfront_embed/app.EmbeddedDirectory.files")
file(READ "${manifest}" listed)
set(expected "@a@ b+~.svg\nempty.css\nindex.html\nsub/deeper/forty-one.txt\nsub/hello.js\n")
if(NOT listed STREQUAL expected)
    message(FATAL_ERROR "accepted: the manifest must list the kept files, sorted:\n${listed}\nexpected:\n${expected}")
endif()

# The renderer, as the build runs it.
set(header "${build}/webfront_embed/app/EmbeddedDirectory.hpp")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=${ui}" "-DMANIFEST=${manifest}" -DCLASS=EmbeddedDirectory
    -DNAMESPACE=webfront::fs "-DOUTPUT=${header}" -P "${ROOT}/cmake/WebFrontEmbedDirectoryRender.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The renderer failed:\n${output}${error}")
endif()
file(READ "${header}" rendered)
foreach(fragment
        "namespace webfront::fs {"
        "class EmbeddedDirectory {"
        "explicit EmbeddedDirectory(std::filesystem::path) { }"
        "if (name == \"@a@ b+~.svg\")\n            return File{Asset0{}};"
        "if (name == \"sub/hello.js\")\n            return File{Asset4{}};"
        "struct Asset1 {  // empty.css\n        static constexpr std::size_t dataSize{0};\n        static constexpr std::array<std::uint64_t, 0> data{};"
        "struct Asset4 {  // sub/hello.js\n        static constexpr std::size_t dataSize{5};\n        static constexpr std::array<std::uint64_t, 1> data{\n            0x68656c6c6f000000\n        };"
        "dataSize{41};\n        static constexpr std::array<std::uint64_t, 6> data{\n            0x3031323334353637, 0x3839303132333435, 0x3637383930313233, 0x3435363738393031, 0x3233343536373839,\n            0x3000000000000000\n        };")
    string(FIND "${rendered}" "${fragment}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "The header lacks:\n${fragment}\n--- header ---\n${rendered}")
    endif()
endforeach()
if(rendered MATCHES "DS_Store|HEAD|notes[.]md|Asset5")
    message(FATAL_ERROR "The header embeds an excluded file:\n${rendered}")
endif()
message(STATUS "accepted: manifest and header as expected")

set(bad "${WORK_DIR}/bad")
file(MAKE_DIRECTORY "${bad}")
file(WRITE "${bad}/bad#name.txt" "x")
expect_failure(name "webfront_embed_directory(app \"${bad}\")" "cannot serve '.*/bad#name[.]txt'.*EXCLUDE")
expect_failure(excluded_name "webfront_embed_directory(app \"${bad}\" EXCLUDE \"#\")
message(FATAL_ERROR \"excluded-ok\")" "excluded-ok")
expect_failure(class "webfront_embed_directory(app \"${ui}\" CLASS 1UI)" "CLASS must be a C[+][+] identifier")
expect_failure(namespace "webfront_embed_directory(app \"${ui}\" NAMESPACE app::)" "NAMESPACE must be a C[+][+] namespace")
expect_failure(directory "webfront_embed_directory(app \"${WORK_DIR}/missing\")" "missing is not a directory")
expect_failure(target "webfront_embed_directory(nothing \"${ui}\")" "nothing is not a target")
expect_failure(keyword "webfront_embed_directory(app \"${ui}\" CLASS)" "keywords without a value: CLASS")
expect_failure(twice "webfront_embed_directory(app \"${ui}\" EXCLUDE \"(^|/)[.]\" \"[.]md$\")
webfront_embed_directory(other::app \"${ui}\" EXCLUDE \"(^|/)[.]\" \"[.]md$\")" "app already embeds a class named EmbeddedDirectory")

# A ';' in a name would split the list of the glob into paths that do not exist, which the build would then miss: each
# position of the semicolon (inside, at either end, in a folder name) and the directory itself are refused at configure.
foreach(case "inside|a;b.txt" "end|sub/end;" "start|sub/;start.txt" "folder|x;y/z.txt")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 label)
    list(SUBLIST case 1 -1 parts)
    list(JOIN parts ";" relative)
    set(semicolon "${WORK_DIR}/semicolon-${label}")
    get_filename_component(parent "${semicolon}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    file(WRITE "${semicolon}/index.html" "x")
    file(WRITE "${semicolon}/${relative}" "x")
    expect_failure(semicolon_${label} "webfront_embed_directory(app \"${semicolon}\")" "cannot serve a file whose path holds ';'.*EXCLUDE")
endforeach()
set(semicolon_directory "${WORK_DIR}/semi;colon")
file(MAKE_DIRECTORY "${semicolon_directory}")
file(WRITE "${semicolon_directory}/index.html" "x")
expect_failure(semicolon_directory "webfront_embed_directory(app \"${semicolon_directory}\")" "cannot embed a directory whose path holds ';'")

# A C++ keyword or a reserved identifier would configure, then the generated header would not compile.
foreach(class class final Asset0 Asset12 A__b)
    expect_failure(class_${class} "webfront_embed_directory(app \"${ui}\" CLASS ${class})" "CLASS cannot be '${class}'")
endforeach()
foreach(namespace and app::int std::module app::__detail _App::ui)
    string(MAKE_C_IDENTIFIER "${namespace}" label)
    expect_failure(namespace_${label} "webfront_embed_directory(app \"${ui}\" NAMESPACE ${namespace})" "NAMESPACE cannot hold '")
endforeach()
