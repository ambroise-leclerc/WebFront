# Public helper that embeds a plain directory (HTML, JavaScript, CSS, images, vendored files) in an application, in pure
# CMake: no developer environment, no JavaScript runtime and no package manager (#675).
include_guard(GLOBAL)

# webfront_embed_directory(<target> <directory> [CLASS <name>] [NAMESPACE <ns>] [EXCLUDE <regex>...])
#
# Generates at build time a header-only provider (ADR-002) for every file of <directory> and its subdirectories, and
# lets <target> include it as <CLASS>.hpp:
#
#   add_subdirectory(WebFront)                                   # or FetchContent / CPM
#   add_executable(app main.cpp)
#   target_link_libraries(app PRIVATE WebFront::WebFront)
#   webfront_embed_directory(app ui CLASS AppUI NAMESPACE app)   # #include <AppUI.hpp>, class app::AppUI
#
# The provider serves each file by its path relative to <directory> ("index.html", "lib/chart.js"), with the bytes of the
# file, identity encoded; the server derives the MIME type from the extension, as for a file on disk. Compose it with
# fs::DefaultFSWith<app::AppUI> for the distributed build, or fs::DocumentRootFirstFSWith<app::AppUI> so that a document
# root given at run time (a --ui <dir> option) is served in front of it during development.
#
# The build regenerates the header when a file of the directory changes, and configures again when a file is added or
# removed (file(GLOB_RECURSE CONFIGURE_DEPENDS)). CLASS defaults to EmbeddedDirectory and NAMESPACE to webfront::fs; a
# C++ keyword or a reserved identifier is refused in either.
# EXCLUDE drops the relative paths that match one of its regular expressions (for instance "(^|/)[.]" for hidden files
# such as .DS_Store). A path may only hold letters, digits and the characters _ . - + @ ~ and space, without an empty,
# "." or ".." part; another name fails the configuration and names the file. A directory reached through a symbolic link
# is not entered.
#
# The header holds every byte uncompressed, as 64-bit words: a few megabytes are fine, a much larger interface compiles
# slowly. `wf embed` generates a gzip-compressed provider for an application built with the developer environment.
function(webfront_embed_directory target directory)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "CLASS;NAMESPACE" "EXCLUDE")
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "webfront_embed_directory: invalid arguments (unknown: ${ARG_UNPARSED_ARGUMENTS}; keywords without a value: ${ARG_KEYWORDS_MISSING_VALUES})")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "webfront_embed_directory: ${target} is not a target")
    endif()
    get_target_property(aliased "${target}" ALIASED_TARGET)
    if(aliased)
        set(target "${aliased}")
    endif()
    if(NOT DEFINED ARG_CLASS)
        set(ARG_CLASS EmbeddedDirectory)
    endif()
    if(NOT DEFINED ARG_NAMESPACE)
        set(ARG_NAMESPACE webfront::fs)
    endif()
    if(NOT ARG_CLASS MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
        message(FATAL_ERROR "webfront_embed_directory: CLASS must be a C++ identifier (got '${ARG_CLASS}')")
    endif()
    if(NOT ARG_NAMESPACE MATCHES "^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$")
        message(FATAL_ERROR "webfront_embed_directory: NAMESPACE must be a C++ namespace such as app or app::ui (got '${ARG_NAMESPACE}')")
    endif()
    # The C++ keywords (C++26 included) and the identifiers with a special meaning (final, override, import, module),
    # between semicolons so that a name matches only as a whole. Defined here: the function runs in its caller's scope.
    set(reserved ";alignas;alignof;and;and_eq;asm;auto;bitand;bitor;bool;break;case;catch;char;char8_t;char16_t;\
char32_t;class;compl;concept;const;consteval;constexpr;constinit;const_cast;continue;contract_assert;co_await;co_return;\
co_yield;decltype;default;delete;do;double;dynamic_cast;else;enum;explicit;export;extern;false;final;float;for;friend;goto;\
if;import;inline;int;long;module;mutable;namespace;new;noexcept;not;not_eq;nullptr;operator;or;or_eq;override;\
private;protected;public;register;reinterpret_cast;requires;return;short;signed;sizeof;static;static_assert;static_cast;\
struct;switch;template;this;thread_local;throw;true;try;typedef;typeid;typename;union;unsigned;using;virtual;void;volatile;\
wchar_t;while;xor;xor_eq;")
    # The header must compile: a keyword, a reserved identifier or the name of a generated member would not.
    if(reserved MATCHES ";${ARG_CLASS};" OR ARG_CLASS MATCHES "__|^Asset[0-9]+$")
        message(FATAL_ERROR "webfront_embed_directory: CLASS cannot be '${ARG_CLASS}': a C++ keyword, a reserved identifier "
            "(with '__') or a name of the generated members (Asset<n>) would not compile")
    endif()
    string(REPLACE "::" ";" components "${ARG_NAMESPACE}")
    foreach(component IN LISTS components)
        if(reserved MATCHES ";${component};" OR component MATCHES "__|^_[A-Z]")
            message(FATAL_ERROR "webfront_embed_directory: NAMESPACE cannot hold '${component}': a C++ keyword or a reserved "
                "identifier (with '__', or '_' and a capital letter) would not compile or could meet a macro")
        endif()
    endforeach()
    cmake_path(ABSOLUTE_PATH directory BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    string(REGEX REPLACE "(.)/$" "\\1" directory "${directory}")
    if(directory MATCHES ";")
        message(FATAL_ERROR "webfront_embed_directory: cannot embed a directory whose path holds ';' (${directory}): CMake "
            "would split its file names. Rename it.")
    endif()
    if(NOT IS_DIRECTORY "${directory}")
        message(FATAL_ERROR "webfront_embed_directory: ${directory} is not a directory")
    endif()

    # The glob is a CMake list, which a ';' in a name splits into fragments: the first keeps the directory, the next ones
    # lose it (or are empty, or the first ends with '/'). They are recognized and refused before any other check, since
    # no fragment names a file and EXCLUDE could not match the whole name.
    file(GLOB_RECURSE found LIST_DIRECTORIES false CONFIGURE_DEPENDS "${directory}/*")
    string(LENGTH "${directory}/" prefix_length)
    set(paths "")
    set(previous "")
    foreach(file IN LISTS found)
        string(FIND "${file}" "${directory}/" at)
        if(NOT at EQUAL 0 OR file MATCHES "/$" OR file STREQUAL "${directory}/")
            message(FATAL_ERROR "webfront_embed_directory: cannot serve a file whose path holds ';' (near '${previous};${file}'): "
                "CMake splits such a name, so neither the build nor EXCLUDE can name it. Rename it.")
        endif()
        string(SUBSTRING "${file}" ${prefix_length} -1 path)
        list(APPEND paths "${path}")
        set(previous "${file}")
    endforeach()
    set(kept "")
    set(sources "")
    foreach(path IN LISTS paths)
        set(excluded OFF)
        foreach(pattern IN LISTS ARG_EXCLUDE)
            if(path MATCHES "${pattern}")
                set(excluded ON)
                break()
            endif()
        endforeach()
        if(excluded)
            continue()
        endif()
        # The characters a C++ string literal, a CMake list and file(WRITE) take as they are, and a URL path can name.
        if(NOT path MATCHES "^[A-Za-z0-9_.@+~ /-]+$" OR path MATCHES "(^|/)[.]?[.]?(/|$)")
            message(FATAL_ERROR "webfront_embed_directory: cannot serve '${directory}/${path}': a path may only hold letters, "
                "digits and _ . - + @ ~ space, with no empty, '.' or '..' part. Rename the file or skip it with EXCLUDE.")
        endif()
        list(APPEND kept "${path}")
        list(APPEND sources "${directory}/${path}")
    endforeach()
    list(SORT kept)
    list(JOIN kept "\n" manifest)

    string(MAKE_C_IDENTIFIER "${target}" name)
    set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/webfront_embed/${name}")
    set(header "${include_dir}/${ARG_CLASS}.hpp")
    set(manifest_file "${CMAKE_CURRENT_BINARY_DIR}/webfront_embed/${name}.${ARG_CLASS}.files")
    # Rewritten only when the list changed: adding or removing a file regenerates the header, a new configure alone does not.
    # Compared by hand: file(CONFIGURE) would substitute an @name@ in a path.
    set(previous "")
    if(EXISTS "${manifest_file}")
        file(READ "${manifest_file}" previous)
    endif()
    if(NOT previous STREQUAL "${manifest}\n")
        file(WRITE "${manifest_file}" "${manifest}\n")
    endif()

    set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WebFrontEmbedDirectoryRender.cmake")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DDIRECTORY=${directory}" "-DMANIFEST=${manifest_file}" "-DCLASS=${ARG_CLASS}"
            "-DNAMESPACE=${ARG_NAMESPACE}" "-DOUTPUT=${header}" -P "${script}"
        DEPENDS ${sources} "${manifest_file}" "${script}"
        COMMENT "Embedding ${directory} as ${ARG_NAMESPACE}::${ARG_CLASS}"
        VERBATIM)
    set(generator "webfront_embed_${name}_${ARG_CLASS}")
    if(TARGET "${generator}")
        message(FATAL_ERROR "webfront_embed_directory: ${target} already embeds a class named ${ARG_CLASS}")
    endif()
    add_custom_target("${generator}" DEPENDS "${header}")
    add_dependencies("${target}" "${generator}")
    get_target_property(type "${target}" TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        target_include_directories("${target}" INTERFACE "${include_dir}")
    else()
        target_include_directories("${target}" PRIVATE "${include_dir}")
    endif()
endfunction()
