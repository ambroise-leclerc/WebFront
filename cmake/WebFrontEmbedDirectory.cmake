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
# removed (file(GLOB_RECURSE CONFIGURE_DEPENDS)). CLASS defaults to EmbeddedDirectory and NAMESPACE to webfront::fs.
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
    cmake_path(ABSOLUTE_PATH directory BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    string(REGEX REPLACE "(.)/$" "\\1" directory "${directory}")
    if(NOT IS_DIRECTORY "${directory}")
        message(FATAL_ERROR "webfront_embed_directory: ${directory} is not a directory")
    endif()

    file(GLOB_RECURSE paths LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${directory}" "${directory}/*")
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
