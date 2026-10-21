# Bootstrap the simply-cpp CMake helpers.
# Include this from a module's top-level CMakeLists.txt before using the helpers.
# Prefer the installed sc-core package, then the repo-local copy, then FetchContent.

set(SC_HELPERS_REPOSITORY "https://github.com/roelofrossouw/simply-cpp.git" CACHE STRING "Where to fetch the simply-cpp build helpers from")
set(SC_HELPERS_TAG "main" CACHE STRING "Which revision of the simply-cpp build helpers to fetch")
option(SC_UPDATE_HELPERS "Track core helper copies" ON)
option(SC_DEPLOY_SCRIPTS "Create/maintain deploy and run scripts" ON)
option(SC_HELPERS_USE_PACKAGE "Use installed sc-core helpers when available" ON)
if (NOT SC_DEPLOY_NAME)
    if (SC_MODULE)
        set(SC_DEPLOY_NAME "${SC_MODULE}")
    else ()
        set(SC_DEPLOY_NAME "${PROJECT_NAME}")
    endif ()
endif ()

set(sc_helpers_cached "${CMAKE_CURRENT_LIST_DIR}/SimplyCppFunctions.cmake")
set(sc_test_header_cached "${CMAKE_CURRENT_SOURCE_DIR}/tests/sc_test.h")
set(sc_bootstrap_cached "${CMAKE_CURRENT_LIST_FILE}") # this file, kept in step with the rest
set(sc_helpers_source "")     # where to copy the helpers from, empty when already cached
set(sc_test_header_source "")
set(sc_bootstrap_source "")
set(sc_scripts_source "")     # directory holding deploy.sh.in and run.sh.in
set(sc_helpers_origin "")

# Reads SC_HELPERS_VERSION out of a helpers file without including it. Absent means 0,
# which is any copy predating the version stamp.
function(sc_helpers_file_version file output)
    set(${output} 0 PARENT_SCOPE)
    if (NOT EXISTS "${file}")
        return()
    endif ()
    file(STRINGS "${file}" version_line REGEX "^set\\(SC_HELPERS_VERSION ")
    if (version_line)
        string(REGEX MATCH "[0-9]+" file_version "${version_line}")
        set(${output} "${file_version}" PARENT_SCOPE)
    endif ()
endfunction()

if (SC_HELPERS_USE_PACKAGE)
    find_package(sc-core QUIET)
endif ()
if (COMMAND get_sc_version)
    set(sc_helpers_origin "the sc-core package at ${sc-core_DIR}")
    if (EXISTS "${sc-core_DIR}/SimplyCppFunctions.cmake")
        set(sc_helpers_source "${sc-core_DIR}/SimplyCppFunctions.cmake")
    endif ()
    if (SC_TEST_INCLUDE_DIR AND EXISTS "${SC_TEST_INCLUDE_DIR}/sc_test.h")
        set(sc_test_header_source "${SC_TEST_INCLUDE_DIR}/sc_test.h")
    endif ()
    if (EXISTS "${sc-core_DIR}/sc_bootstrap.cmake")
        set(sc_bootstrap_source "${sc-core_DIR}/sc_bootstrap.cmake")
    endif ()
    file(GLOB sc_installed_templates "${sc-core_DIR}/*.sh.in")
    if (sc_installed_templates)
        set(sc_scripts_source "${sc-core_DIR}")
    endif ()
endif ()

if (COMMAND get_sc_version AND EXISTS "${sc_helpers_cached}")
    sc_helpers_file_version("${sc_helpers_source}" sc_installed_helpers_version)
    sc_helpers_file_version("${sc_helpers_cached}" sc_local_helpers_version)
    if (sc_local_helpers_version GREATER sc_installed_helpers_version)
        message(WARNING "Installed helpers are older than this module; using local copy instead.")
        include("${sc_helpers_cached}")
        set(sc_helpers_origin "${sc_helpers_cached}")
        set(sc_helpers_source "")
        set(sc_test_header_source "")
        set(sc_bootstrap_source "")
        set(sc_scripts_source "")
    endif ()
endif ()

if (NOT COMMAND get_sc_version AND EXISTS "${sc_helpers_cached}")
    include("${sc_helpers_cached}")
    set(sc_helpers_origin "${sc_helpers_cached}")
endif ()

if (NOT COMMAND get_sc_version)
    message(STATUS "No sc package and no cached helpers, fetching from ${SC_HELPERS_REPOSITORY}")
    include(FetchContent)
    FetchContent_Declare(sc_helpers
            GIT_REPOSITORY ${SC_HELPERS_REPOSITORY}
            GIT_TAG ${SC_HELPERS_TAG}
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sc-helpers-are-not-built)
    FetchContent_MakeAvailable(sc_helpers)

    set(sc_helpers_source "${sc_helpers_SOURCE_DIR}/cmake/SimplyCppFunctions.cmake")
    set(sc_test_header_source "${sc_helpers_SOURCE_DIR}/tests/sc_test.h")
    set(sc_bootstrap_source "${sc_helpers_SOURCE_DIR}/cmake/sc_bootstrap.cmake")
    set(sc_scripts_source "${sc_helpers_SOURCE_DIR}/scripts")
    set(sc_helpers_origin "${SC_HELPERS_REPOSITORY}@${SC_HELPERS_TAG}")
    include("${sc_helpers_source}")
endif ()

if (NOT COMMAND get_sc_version)
    message(FATAL_ERROR "Found simply-cpp build helpers at ${sc_helpers_origin} but they do not"
            " define get_sc_version(). They predate it - update the copy with"
            " -DSC_UPDATE_HELPERS=ON, or reinstall simply-cpp core.")
endif ()

message(STATUS "simply-cpp build helpers from ${sc_helpers_origin}")

# Cache the helper copies so later offline builds still work.
function(sc_cache_helper source destination what)
    if (NOT source OR NOT EXISTS "${source}")
        return()
    endif ()
    get_filename_component(destination_dir "${destination}" DIRECTORY)
    if (NOT IS_DIRECTORY "${destination_dir}")
        return()
    endif ()

    get_filename_component(source_path "${source}" REALPATH)
    get_filename_component(destination_path "${destination}" ABSOLUTE)
    if (source_path STREQUAL destination_path)
        return()
    endif ()

    if (EXISTS "${destination}")
        file(SHA256 "${source}" source_hash)
        file(SHA256 "${destination}" destination_hash)
        if (source_hash STREQUAL destination_hash)
            return()
        endif ()
        if (NOT SC_UPDATE_HELPERS)
            message(STATUS "${what} differs from ${sc_helpers_origin}; pinned copy kept")
            set(sc_helpers_drifted TRUE PARENT_SCOPE)
            return()
        endif ()
    endif ()

    file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT RESULT copy_error)
    if (copy_error)
        message(WARNING "Could not cache ${what} to ${destination}: ${copy_error}")
    else ()
        message(STATUS "Cached ${what} to ${destination}")
    endif ()
endfunction()

set(sc_helpers_drifted FALSE)
sc_cache_helper("${sc_helpers_source}" "${sc_helpers_cached}" "SimplyCppFunctions.cmake")
sc_cache_helper("${sc_test_header_source}" "${sc_test_header_cached}" "sc_test.h")
sc_cache_helper("${sc_bootstrap_source}" "${sc_bootstrap_cached}" "sc_bootstrap.cmake")

if (SC_DEPLOY_SCRIPTS)
    set(sc_module_scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    file(MAKE_DIRECTORY "${sc_module_scripts}")

    if (sc_scripts_source)
        file(GLOB sc_core_templates "${sc_scripts_source}/*.sh.in")
        foreach (template ${sc_core_templates})
            get_filename_component(template_name "${template}" NAME)
            sc_cache_helper("${template}" "${sc_module_scripts}/${template_name}" "${template_name}")
        endforeach ()
    endif ()

    file(GLOB sc_module_templates "${sc_module_scripts}/*.sh.in")
    foreach (template ${sc_module_templates})
        get_filename_component(script "${template}" NAME_WLE)
        configure_file("${template}" "${CMAKE_CURRENT_BINARY_DIR}/sc-scripts/${script}" @ONLY)
        sc_cache_helper("${CMAKE_CURRENT_BINARY_DIR}/sc-scripts/${script}"
                "${sc_module_scripts}/${script}" "${script}")
        if (EXISTS "${sc_module_scripts}/${script}")
            file(CHMOD "${sc_module_scripts}/${script}"
                    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endif ()
    endforeach ()
endif ()

if (sc_helpers_drifted)
    message(STATUS "The refreshed helpers take effect on the next configure")
endif ()

