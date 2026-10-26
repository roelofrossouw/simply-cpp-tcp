# Keep project()/cmake_minimum_required() in the top-level CMakeLists.txt.
# Only the post-project C++/PIC defaults are safe here.
# This file is also loaded by downstream consumers via find_package(), where SC_MODULE is unset.
if (SC_MODULE)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    option(SC_BUILD_EXAMPLES "Build and install the module's examples" ON)
endif ()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(CMakeParseArguments)

# Keep runtime search paths valid for relocatable installs.
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if (APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif (UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif ()

set(SC_HELPERS_VERSION 31)
set(SC_VERSION_FILE "VERSION.txt")
set(SC_VERSION_DEFAULT "1.0.0")

function(parse_sc_version text output)
    string(REGEX REPLACE "^[vV]" "" candidate "${text}")
    # CMake regexes have no {m,n} repetition, so the optional parts are spelled out.
    if (candidate MATCHES "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?$")
        set(${output} "${candidate}" PARENT_SCOPE)
    else ()
        set(${output} "" PARENT_SCOPE)
    endif ()
endfunction()

function(get_sc_version)
    set(version_file "${CMAKE_CURRENT_SOURCE_DIR}/${SC_VERSION_FILE}")
    set(version "")

    find_package(Git QUIET)
    if (Git_FOUND)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE git_tag
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE git_result
        )

        if (git_result EQUAL 0)
            parse_sc_version("${git_tag}" version)
            if ("${version}" STREQUAL "")
                message(WARNING "Ignoring git tag '${git_tag}': not a version number")
            endif ()
        endif ()
    endif ()

    if (NOT "${version}" STREQUAL "")
        message(STATUS "Version ${version} from git tag '${git_tag}'")
        write_sc_version_file("${version}" "${version_file}")
    else ()
        read_sc_version_file("${version_file}" version)
    endif ()

    if ("${version}" STREQUAL "")
        set(version "${SC_VERSION_DEFAULT}")
        message(WARNING "No usable git tag and no ${SC_VERSION_FILE}; defaulting to ${version}.")
    endif ()

    set(SC_VERSION "${version}" PARENT_SCOPE)
endfunction()

function(write_sc_version_file version version_file)
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/${SC_VERSION_FILE}")
    file(WRITE "${staged}" "${version}\n")
    file(COPY_FILE "${staged}" "${version_file}" ONLY_IF_DIFFERENT RESULT copy_error)
    if (copy_error)
        message(WARNING "Could not update ${version_file}: ${copy_error}."
                " A build without git will fall back to whatever it already holds.")
    endif ()
endfunction()

function(read_sc_version_file version_file output)
    set(${output} "" PARENT_SCOPE)
    if (NOT EXISTS "${version_file}")
        return()
    endif ()

    file(READ "${version_file}" contents)
    string(STRIP "${contents}" contents)
    parse_sc_version("${contents}" version)
    if ("${version}" STREQUAL "")
        message(WARNING "Ignoring ${version_file}: '${contents}' is not a version number")
        return()
    endif ()

    message(STATUS "Version ${version} from ${SC_VERSION_FILE} (no usable git tag)")
    set(${output} "${version}" PARENT_SCOPE)
endfunction()

# Try the requested package name and the lowercase form, and report success under the caller's spelling.
macro(sc_find_package_any_case package)
    find_package(${package} QUIET ${ARGN})

    string(TOLOWER "${package}" SC_ANY_CASE_LOWER)
    if (NOT ${package}_FOUND AND NOT "${SC_ANY_CASE_LOWER}" STREQUAL "${package}")
        find_package(${SC_ANY_CASE_LOWER} QUIET ${ARGN})
        if (${SC_ANY_CASE_LOWER}_FOUND)
            message(STATUS "${package} resolved as ${SC_ANY_CASE_LOWER}")
            set(${package}_FOUND TRUE)
            if (NOT ${package}_VERSION)
                set(${package}_VERSION "${${SC_ANY_CASE_LOWER}_VERSION}")
            endif ()
        endif ()
    endif ()
endmacro()

# Ensure the simply-cpp package source is registered before installing our own packages.
function(sc_ensure_package_source)
    if (APPLE)
        execute_process(COMMAND brew --repository OUTPUT_VARIABLE SC_BREW_REPO OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (NOT SC_BREW_REPO OR EXISTS "${SC_BREW_REPO}/Library/Taps/roelofrossouw/homebrew-sc")
            return()
        endif ()
    elseif (UNIX AND EXISTS "/usr/bin/apt")
        if (EXISTS "/etc/apt/sources.list.d/simply-cpp.list")
            return()
        endif ()
    else ()
        return()
    endif ()

    message(STATUS "Registering the simply-cpp package source")
    execute_process(COMMAND bash -c "curl -fsSL https://apt.roelof.co.za/setup.sh | bash")
endfunction()

# find_or_install_package(<package> <apt name> <brew name> [COMPONENTS ...])
# A macro so find_package() result variables land in the caller's scope.
macro(find_or_install_package package apt_name brew_name)
    cmake_parse_arguments(SC_PACKAGE "" "" "COMPONENTS" ${ARGN})
    set(SC_PACKAGE_ARGS)
    if (SC_PACKAGE_COMPONENTS)
        set(SC_PACKAGE_ARGS COMPONENTS ${SC_PACKAGE_COMPONENTS})
    endif ()

    message(STATUS "Detecting ${package}")
    #sc_find_package_any_case(${package} ${SC_PACKAGE_ARGS})
    find_package(${package} QUIET ${ARGN})

    if (NOT ${package}_FOUND)
        sc_ensure_package_source()
        if (UNIX AND EXISTS "/usr/bin/apt")
            message(STATUS "${package} not found, attempting apt installation...")
            execute_process(COMMAND sudo apt -y install ${apt_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()
        if (APPLE)
            message(STATUS "${package} not found, attempting brew installation...")
            execute_process(COMMAND brew install ${brew_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()

        # Re-check the package that was asked for, at the version asked for: what apt or brew had
        # may be older than that (a release not published yet).
        sc_find_package_any_case(${package} ${ARGN})
        if (NOT ${package}_FOUND)
            set(SC_PACKAGE_WANTED "")
            set(SC_PACKAGE_EXTRA ${ARGN}) # a macro's ARGN isn't a variable if() or list() can read
            if (SC_PACKAGE_EXTRA)
                list(GET SC_PACKAGE_EXTRA 0 SC_PACKAGE_WANTED)
                if (NOT SC_PACKAGE_WANTED MATCHES "^[0-9]")
                    set(SC_PACKAGE_WANTED "")
                endif ()
            endif ()
            sc_find_package_any_case(${package} ${SC_PACKAGE_ARGS}) # any version: which there is
            if (${package}_FOUND AND SC_PACKAGE_WANTED)
                message(FATAL_ERROR "${package} ${${package}_VERSION} is installed, but ${PROJECT_NAME} needs"
                        " ${SC_PACKAGE_WANTED} or later: install a newer ${apt_name} (or build ${package} and install it)")
            endif ()
            message(FATAL_ERROR "Failed to install or locate ${package}"
                    " (install result=${SC_PACKAGE_INSTALL_RESULT})")
        endif ()
    endif ()

    message(STATUS "${package} found - ${${package}_VERSION}")
endmacro()

# sc_forget_stale_pkg_config(<prefix>)
# pkg_check_modules() keeps what it found in the cache and doesn't look again. Homebrew keeps
# each version of a package in its own Cellar folder and removes the old one on upgrade, so a
# cached library path can stop existing (make: "No rule to make target .../Cellar/<package>/
# <old version>/lib/..."). When one has, forget the result so pkg-config is asked again.
function(sc_forget_stale_pkg_config prefix)
    get_cmake_property(cached_variables CACHE_VARIABLES)
    # Only pkg-config's own entries: reading every cached variable would read CMake's too, and
    # CMake 4.4 warns about code that reads CMAKE_WARN_DEPRECATED (policy CMP0218).
    set(libraries ${cached_variables})
    list(FILTER libraries INCLUDE REGEX "^pkgcfg_lib_${prefix}_")
    set(results ${cached_variables})
    list(FILTER results INCLUDE REGEX "^${prefix}_(STATIC_)?[A-Z_]+$")
    set(stale FALSE)
    foreach (variable IN LISTS libraries)
        if (${variable} AND NOT EXISTS "${${variable}}")
            set(stale TRUE)
        endif ()
    endforeach ()
    # The folders it found, the packages it depends on among them: a Homebrew upgrade of one of
    # those (brotli under freetype, say) removes its versioned Cellar folder, and the imported
    # target then names a folder that isn't there.
    foreach (folders IN ITEMS ${prefix}_INCLUDE_DIRS ${prefix}_LIBRARY_DIRS ${prefix}_STATIC_INCLUDE_DIRS ${prefix}_STATIC_LIBRARY_DIRS)
        foreach (folder IN LISTS ${folders})
            if (folder AND NOT EXISTS "${folder}")
                set(stale TRUE)
            endif ()
        endforeach ()
    endforeach ()
    if (NOT stale)
        return()
    endif ()
    message(STATUS "${prefix}: a cached location is gone (upgraded?), looking again")
    foreach (variable IN LISTS libraries results)
        unset(${variable} CACHE)
    endforeach ()
    unset(__pkg_config_checked_${prefix} CACHE)
endfunction()

macro(config_or_install_package package apt_name brew_name)
    cmake_parse_arguments(SC_PACKAGE "" "" "COMPONENTS" ${ARGN})
    set(SC_PACKAGE_ARGS)
    if (SC_PACKAGE_COMPONENTS)
        set(SC_PACKAGE_ARGS COMPONENTS ${SC_PACKAGE_COMPONENTS})
    endif ()
    string(TOLOWER "${package}" package_lower)

    message(STATUS "Detecting ${package}")
    find_package(PkgConfig REQUIRED)
    sc_forget_stale_pkg_config(${package})
    pkg_check_modules(${package} IMPORTED_TARGET ${package_lower})

    # Tesseract ships pkg-config rather than a CMake config on both platforms.
    #find_package(PkgConfig REQUIRED)
    #pkg_check_modules(Tesseract IMPORTED_TARGET tesseract)
    #if (NOT Tesseract_FOUND)
    #    if (UNIX AND EXISTS "/usr/bin/apt")
    #        message(STATUS "tesseract not found, attempting apt installation...")
    #        execute_process(COMMAND sudo apt -y install libtesseract-dev)
    #    elseif (APPLE)
    #        message(STATUS "tesseract not found, attempting brew installation...")
    #        execute_process(COMMAND brew install tesseract)
    #    endif ()
    #    pkg_check_modules(Tesseract REQUIRED IMPORTED_TARGET tesseract)
    #endif ()
    #message(STATUS "Tesseract found - ${Tesseract_VERSION}")



    if (NOT ${package}_FOUND)
        sc_ensure_package_source()
        if (UNIX AND EXISTS "/usr/bin/apt")
            message(STATUS "${package} not found, attempting apt installation...")
            execute_process(COMMAND sudo apt -y install ${apt_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()
        if (APPLE)
            message(STATUS "${package} not found, attempting brew installation...")
            execute_process(COMMAND brew install ${brew_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()

        # Re-check the package that was asked for.
        pkg_check_modules(${package} IMPORTED_TARGET ${package_lower})
        if (NOT ${package}_FOUND)
            message(FATAL_ERROR "Failed to install or configure ${package}"
                    " (install result=${SC_PACKAGE_INSTALL_RESULT})")
        endif ()
    endif ()

    message(STATUS "${package} found - ${${package}_VERSION}")
endmacro()

# find_or_fetch_package(<package> ...)
# Use an installed package when present; otherwise fetch it.
macro(find_or_fetch_package package)
    cmake_parse_arguments(SC_FETCH "" "GIT_REPOSITORY;GIT_TAG;VERSION;FORCE" "COMPONENTS;DECLARE_ARGS" ${ARGN})

    set(SC_FETCH_FIND_ARGS ${SC_FETCH_VERSION})
    if (SC_FETCH_COMPONENTS)
        list(APPEND SC_FETCH_FIND_ARGS COMPONENTS ${SC_FETCH_COMPONENTS})
    endif ()

    if (SC_FETCH_FORCE)
        set(${package}_FOUND FALSE)
    else ()
        message(STATUS "Detecting ${package}")
        sc_find_package_any_case(${package} ${SC_FETCH_FIND_ARGS})
    endif ()

    if (${package}_FOUND)
        message(STATUS "${package} found - ${${package}_VERSION}")
    else ()
        if (NOT SC_FETCH_GIT_REPOSITORY)
            message(FATAL_ERROR "find_or_fetch_package(${package}): not installed,"
                    " and no GIT_REPOSITORY given to fetch it from")
        endif ()
        if (NOT SC_FETCH_GIT_TAG)
            set(SC_FETCH_GIT_TAG main)
        endif ()
        message(STATUS "Fetching ${package} - ${SC_FETCH_GIT_REPOSITORY}@${SC_FETCH_GIT_TAG}")
        FetchContent_Declare(${package}
                GIT_REPOSITORY ${SC_FETCH_GIT_REPOSITORY}
                GIT_TAG ${SC_FETCH_GIT_TAG}
                GIT_SHALLOW TRUE
                EXCLUDE_FROM_ALL
                ${SC_FETCH_DECLARE_ARGS})
        FetchContent_MakeAvailable(${package})
    endif ()
endmacro()

# add_sc_object(<name> ...)
# Builds src/<name>.cpp into an object library for the module's consolidated libraries.
function(add_sc_object object)
    # sc-obj-, not sc-: sc-<module> is the final library.
    set(object_name "sc-obj-${object}")
    set(source_file "src/${object}.cpp")
    set(header_file "include/${object}.h")

    set(options)
    set(one_value_args)
    set(multi_value_args SOURCES INCLUDE_DIRS LINK_LIBRARIES PUBLIC_LINK_LIBRARIES)
    cmake_parse_arguments(SC_OBJECT "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    set(INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_library(${object_name} OBJECT ${source_file} ${header_file} ${SC_OBJECT_SOURCES})
    set_target_properties(${object_name} PROPERTIES EXCLUDE_FROM_ALL ON)
    target_include_directories(${object_name} PRIVATE ${INCLUDE} ${SC_OBJECT_INCLUDE_DIRS})

    if (SC_OBJECT_LINK_LIBRARIES)
        target_link_libraries(${object_name} PRIVATE ${SC_OBJECT_LINK_LIBRARIES})
    endif ()

    list(APPEND SOURCE_OBJECTS $<TARGET_OBJECTS:${object_name}>)
    set(SOURCE_OBJECTS "${SOURCE_OBJECTS}" PARENT_SCOPE)

    if (SC_OBJECT_PUBLIC_LINK_LIBRARIES)
        list(APPEND SOURCE_LINK_LIBRARIES ${SC_OBJECT_PUBLIC_LINK_LIBRARIES})
        set(SOURCE_LINK_LIBRARIES "${SOURCE_LINK_LIBRARIES}" PARENT_SCOPE)
    endif ()
endfunction()

# sc_module_name(<output>)
# Returns the module library/package name: sc-<SC_MODULE>.
function(sc_module_name output)
    if (NOT SC_MODULE)
        message(FATAL_ERROR "SC_MODULE is not set."
                " Set it before add_sc_libraries()/install_sc_module(), or pass NAME.")
    endif ()
    set(${output} "sc-${SC_MODULE}" PARENT_SCOPE)
endfunction()

# add_sc_libraries([NAME ...])
# Build the static and shared module libraries from SOURCE_OBJECTS.
function(add_sc_libraries)
    set(one_value_args NAME DESCRIPTION VERSION INCLUDE_DIR)
    cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

    set(name "${ARG_NAME}")
    if (NOT name)
        sc_module_name(name)
    endif ()
    if (NOT ARG_VERSION)
        set(ARG_VERSION "${SC_VERSION}")
    endif ()
    if (NOT ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    endif ()
    if (NOT ARG_DESCRIPTION)
        set(ARG_DESCRIPTION "simply-cpp ${name}")
    endif ()
    string(REGEX MATCH "^[0-9]+" version_major "${ARG_VERSION}")

    # Captured before the new targets are added, so they link the module's dependencies
    # and not each other.
    set(dependencies ${SOURCE_LIBRARIES} ${SOURCE_LINK_LIBRARIES})

    foreach (kind STATIC SHARED)
        set(target "${name}")
        set(what "consolidated static library")
        if (kind STREQUAL "SHARED")
            set(target "${name}-shared")
            set(what "common shared library")
        endif ()

        add_library(${target} ${kind} ${SOURCE_OBJECTS})
        add_library(sc::${target} ALIAS ${target})
        set_target_properties(${target} PROPERTIES
                VERSION ${ARG_VERSION}
                SOVERSION ${version_major}
                DESCRIPTION "${ARG_DESCRIPTION} ${what}")
        target_include_directories(${target} PUBLIC
                $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_link_libraries(${target} PUBLIC ${dependencies})
        list(APPEND SOURCE_LIBRARIES ${target})
        if (${kind} STREQUAL SHARED)
            # Export the shared library from the call that installs its real file.
            install(TARGETS ${target} EXPORT ${name}Targets
                    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime NAMELINK_COMPONENT development)
        endif ()
    endforeach ()

    set(SOURCE_LIBRARIES "${SOURCE_LIBRARIES}" PARENT_SCOPE)
endfunction()

# install_sc_module([NAME ...])
# Install headers and package config for consumers.
function(install_sc_module)
    set(one_value_args NAME VERSION CONFIG_TEMPLATE)
    set(multi_value_args PATH_VARS)
    cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(name "${ARG_NAME}")
    if (NOT name)
        sc_module_name(name)
    endif ()
    if (NOT ARG_VERSION)
        set(ARG_VERSION "${SC_VERSION}")
    endif ()
    if (NOT ARG_CONFIG_TEMPLATE)
        set(ARG_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${name}Config.cmake.in")
    endif ()
    if (NOT EXISTS "${ARG_CONFIG_TEMPLATE}")
        message(FATAL_ERROR "install_sc_module(${name}): no package config template at ${ARG_CONFIG_TEMPLATE}")
    endif ()

    set(package_destination "${CMAKE_INSTALL_LIBDIR}/cmake/${name}")

    # Install only the static archive here; the shared library is exported from its own install() call.
    set(sc_static_libraries "${SOURCE_LIBRARIES}")
    list(FILTER sc_static_libraries EXCLUDE REGEX "-shared$")
    install(TARGETS ${sc_static_libraries} EXPORT ${name}Targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ COMPONENT development DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN ".DS_Store" EXCLUDE)
    install(EXPORT ${name}Targets FILE ${name}Targets.cmake NAMESPACE sc:: DESTINATION ${package_destination} COMPONENT development)

    configure_package_config_file(${ARG_CONFIG_TEMPLATE} ${CMAKE_CURRENT_BINARY_DIR}/${name}Config.cmake
            INSTALL_DESTINATION ${package_destination}
            PATH_VARS ${ARG_PATH_VARS})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/${name}ConfigVersion.cmake
            VERSION ${ARG_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${name}Config.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/${name}ConfigVersion.cmake
            DESTINATION ${package_destination} COMPONENT development)
endfunction()

# package_sc_module([DEPENDS ...] [DEV_DEPENDS ...] [BREAKS ...] [REPLACES ...] [DEV_BREAKS ...]
#                   [DEV_REPLACES ...])
# Set Debian runtime/dev dependency metadata for the generated package. BREAKS and REPLACES (and
# their DEV_ forms, for the -dev package) let a package take over files another one shipped, or
# make apt upgrade packages together: "simply-cpp-graphics-dev (<< 3.0.0~)".
function(package_sc_module)
    cmake_parse_arguments(ARG "" "" "DEPENDS;DEV_DEPENDS;BREAKS;REPLACES;DEV_BREAKS;DEV_REPLACES" ${ARGN})
    # cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    if (APPLE)
        set(CODENAME apple)
        set(CPACK_GENERATOR "TGZ")
    elseif (UNIX)
        execute_process(
                COMMAND lsb_release -sc
                OUTPUT_VARIABLE CODENAME
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(CPACK_GENERATOR "DEB")
    endif ()
    if (${SC_MODULE} STREQUAL "core")
        set(SC_PACKAGE_NAME simply-cpp)
    else ()
        set(SC_PACKAGE_NAME simply-cpp-${SC_MODULE})
    endif ()
    set(CPACK_COMPONENTS_ALL runtime development)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_PACKAGE_NAME "${SC_PACKAGE_NAME}")
    set(CPACK_PACKAGE_VERSION "${SC_VERSION}~${CODENAME}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simply C++ ${SC_MODULE} module")
    set(CPACK_PACKAGE_CONTACT "Roelof Rossouw")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "${SC_PACKAGE_NAME}")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "${SC_PACKAGE_NAME}-dev")
    # set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}-unknown-${CPACK_PACKAGE_VERSION}-${CODENAME}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_RUNTIME_FILE_NAME "${SC_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_DEVELOPMENT_FILE_NAME "${SC_PACKAGE_NAME}-dev-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Roelof Rossouw <roelof@roelof.co.za>")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Simply C++ ${SC_MODULE} module for Ubuntu ${CODENAME}")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_LIBRARY_PACKAGE_SECTION "libs")
    if (ARG_DEPENDS)
        string(REPLACE ";" ", " ARG_DEPENDS_LIST "${ARG_DEPENDS}")
        set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "${ARG_DEPENDS_LIST}")
        # set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS "${ARG_DEPENDS_LIST}")
    endif ()
    # The licence, where Debian keeps one: /usr/share/doc/<package>/copyright, in both packages.
    # sc::params' --license-text reads it (sc_program_info()).
    sc_license_file(license_file)
    if (license_file)
        install(FILES "${license_file}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${SC_PACKAGE_NAME}
                RENAME copyright COMPONENT runtime)
        install(FILES "${license_file}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${SC_PACKAGE_NAME}-dev
                RENAME copyright COMPONENT development)
    endif ()
    # The Apache License asks that its NOTICE go along with the work.
    if (EXISTS "${PROJECT_SOURCE_DIR}/NOTICE")
        install(FILES "${PROJECT_SOURCE_DIR}/NOTICE" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${SC_PACKAGE_NAME} COMPONENT runtime)
        install(FILES "${PROJECT_SOURCE_DIR}/NOTICE" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${SC_PACKAGE_NAME}-dev COMPONENT development)
    endif ()
    if (PROJECT_HOMEPAGE_URL)
        set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")
    endif ()
    set(development_depends "${SC_PACKAGE_NAME} (= ${CPACK_PACKAGE_VERSION})")
    if (ARG_DEV_DEPENDS)
        list(APPEND development_depends ${ARG_DEV_DEPENDS})
    endif ()
    string(REPLACE ";" ", " development_depends "${development_depends}")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS "${development_depends}")
    foreach (field BREAKS REPLACES)
        if (ARG_${field})
            string(REPLACE ";" ", " list "${ARG_${field}}")
            set(CPACK_DEBIAN_RUNTIME_PACKAGE_${field} "${list}")
        endif ()
        if (ARG_DEV_${field})
            string(REPLACE ";" ", " list "${ARG_DEV_${field}}")
            set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_${field} "${list}")
        endif ()
    endforeach ()
    include(CPack)
endfunction()

# add_sc_test(<name> ...)
# Build a CTest target from <name>.cpp.
function(add_sc_test name)
    # Prefix ARG to avoid clobbering the directory-level SC_TEST_LINK_LIBRARIES default.
    set(options)
    set(one_value_args TIMEOUT)
    set(multi_value_args LABELS LINK_LIBRARIES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif ()

    if (NOT ARG_LINK_LIBRARIES)
        set(ARG_LINK_LIBRARIES ${SC_TEST_LINK_LIBRARIES})
    endif ()
    if (NOT ARG_LINK_LIBRARIES)
        message(FATAL_ERROR "add_sc_test(${name}): nothing to link against."
                " Pass LINK_LIBRARIES, or set SC_TEST_LINK_LIBRARIES for the directory.")
    endif ()

    set(target "test-${name}")
    add_executable(${target} "${name}.cpp")
    target_link_libraries(${target} PRIVATE ${ARG_LINK_LIBRARIES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SC_TEST_INCLUDE_DIR})

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            TIMEOUT ${ARG_TIMEOUT}
            LABELS "${ARG_LABELS}")
endfunction()
# add_sc_example(<name> [SOURCE <file>] [LINK_LIBRARIES ...])
# Build examples/<name>.cpp against the module's shared library and install it with the
# runtime package, so `apt install <package>` alone is enough to run it. Name examples
# sc-<module>-<what> since they land in bin/. A demo is for people trying the module, so it
# prints what it does rather than checking anything, and is not registered with CTest.
# README.md code blocks marked with <!-- sc-example: <file> --> are refreshed from the source.
function(add_sc_example name)
    set(one_value_args SOURCE TIMEOUT)
    set(multi_value_args LINK_LIBRARIES ARGS)
    cmake_parse_arguments(ARG "RUN_AS_TEST" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if (ARG_RUN_AS_TEST OR ARG_TIMEOUT OR ARG_ARGS)
        message(WARNING "add_sc_example(${name}): demos are not tests, so RUN_AS_TEST, ARGS and TIMEOUT"
                " are ignored; drop them")
    endif ()

    sc_module_name(module)
    if (NOT name MATCHES "^${module}-")
        message(WARNING "add_sc_example(${name}): examples install to bin/, name it ${module}-<what>")
    endif ()
    if (NOT ARG_SOURCE)
        set(ARG_SOURCE "examples/${name}.cpp")
    endif ()
    if (NOT ARG_LINK_LIBRARIES)
        set(ARG_LINK_LIBRARIES sc::${module}-shared)
    endif ()

    sc_sync_readme_later()
    if (NOT SC_BUILD_EXAMPLES)
        return()
    endif ()

    add_executable(${name} "${ARG_SOURCE}")
    target_link_libraries(${name} PRIVATE ${ARG_LINK_LIBRARIES})
    sc_program_info(${name}) # --license and --license-text for a demo that uses sc::params
    install(TARGETS ${name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
endfunction()

# sc_package_name(<out>)
# The runtime package's name: simply-cpp for sc-core, simply-cpp-<module> for a module, else the
# project's name (an app such as oneapi).
function(sc_package_name out)
    if (CPACK_PACKAGE_NAME)
        set(${out} ${CPACK_PACKAGE_NAME} PARENT_SCOPE) # an app's own (html2pdf, oneapi)
    elseif (SC_MODULE STREQUAL "core")
        set(${out} simply-cpp PARENT_SCOPE)
    elseif (SC_MODULE)
        set(${out} simply-cpp-${SC_MODULE} PARENT_SCOPE)
    else ()
        set(${out} ${PROJECT_NAME} PARENT_SCOPE)
    endif ()
endfunction()

# sc_license_file(<out>)
# The project's licence file (LICENSE, LICENSE.txt, LICENSE.md or COPYING), "" when it has none.
function(sc_license_file out)
    foreach (candidate LICENSE LICENSE.txt LICENSE.md COPYING)
        if (EXISTS "${PROJECT_SOURCE_DIR}/${candidate}")
            set(${out} "${PROJECT_SOURCE_DIR}/${candidate}" PARENT_SCOPE)
            return()
        endif ()
    endforeach ()
    set(${out} "" PARENT_SCOPE)
endfunction()

# sc_license_name(<out> <file>)
# The licence's SPDX name from the start of its text ("Apache-2.0", "AGPL-3.0", "MIT", ...), or
# "see <file name>" when it isn't one of the common ones.
function(sc_license_name out file)
    file(READ "${file}" head LIMIT 4000)
    string(TOUPPER "${head}" head)
    string(REGEX REPLACE "[ \t\r\n]+" " " head "${head}")
    get_filename_component(file_name "${file}" NAME)
    set(name "see ${file_name}")
    if (head MATCHES "GNU AFFERO GENERAL PUBLIC LICENSE")
        set(name AGPL-3.0)
    elseif (head MATCHES "GNU LESSER GENERAL PUBLIC LICENSE")
        if (head MATCHES "VERSION 2\\.1")
            set(name LGPL-2.1)
        else ()
            set(name LGPL-3.0)
        endif ()
    elseif (head MATCHES "GNU GENERAL PUBLIC LICENSE")
        if (head MATCHES "VERSION 2,")
            set(name GPL-2.0)
        else ()
            set(name GPL-3.0)
        endif ()
    elseif (head MATCHES "APACHE LICENSE" AND head MATCHES "VERSION 2\\.0")
        set(name Apache-2.0)
    elseif (head MATCHES "MOZILLA PUBLIC LICENSE" AND head MATCHES "VERSION 2\\.0")
        set(name MPL-2.0)
    elseif (head MATCHES "MIT LICENSE" OR head MATCHES "PERMISSION IS HEREBY GRANTED, FREE OF CHARGE")
        set(name MIT)
    elseif (head MATCHES "REDISTRIBUTION AND USE IN SOURCE AND BINARY FORMS")
        set(name BSD)
    elseif (head MATCHES "THIS IS FREE AND UNENCUMBERED SOFTWARE")
        set(name Unlicense)
    endif ()
    set(${out} "${name}" PARENT_SCOPE)
endfunction()

# sc_man_page(<target> [EXTRA <file.md>] [COMPONENT <component>])
# The program's man page, section 1: sc::params writes it from the structure that makes its --help
# (params.h, man_page()), when the built program runs with SC_PARAMS_MAN_PAGE set. EXTRA is a file
# of extra sections in a little Markdown (EXAMPLES, SEE ALSO, AUTHORS ...; a DESCRIPTION adds to
# the generated one), by default man/<target>.md when there is one. The page is made at build
# time, so it can't drift from --help, and installed into share/man/man1 (COMPONENT, when given).
function(sc_man_page target)
    cmake_parse_arguments(ARG "" "EXTRA;COMPONENT" "" ${ARGN})
    set(extra "${ARG_EXTRA}")
    if (NOT extra AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/man/${target}.md")
        set(extra "${CMAKE_CURRENT_SOURCE_DIR}/man/${target}.md")
    endif ()
    set(page "${CMAKE_CURRENT_BINARY_DIR}/${target}.1")
    add_custom_command(OUTPUT "${page}"
            COMMAND ${CMAKE_COMMAND} -E env "SC_PARAMS_MAN_PAGE=$<IF:$<BOOL:${extra}>,${extra},1>" sh -c "'$<TARGET_FILE:${target}>' > '${page}'"
            DEPENDS ${target} ${extra}
            COMMENT "Writing the man page ${target}.1"
            VERBATIM)
    add_custom_target(${target}-man ALL DEPENDS "${page}")
    if (ARG_COMPONENT)
        install(FILES "${page}" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT ${ARG_COMPONENT})
    else ()
        install(FILES "${page}" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
    endif ()
endfunction()

# sc_program_info(<target> [LICENSE <name>] [URL <url>] [TEXT_URL <url>])
# Records the project's licence in the program at compile time, for sc::params' --license and
# --license-text (params.h). It generates a header with the licence's name, where to read it and
# where to fetch its text, and the files that may hold the text: the copyright file the package
# installs (package_sc_module()), and the source's LICENSE. The name comes from the LICENSE file,
# the link from the project's HOMEPAGE_URL (or else its git remote): on GitHub, the LICENSE of
# this version's tag (v<version>), else spdx.org's page for the licence. Without a licence file
# (and no LICENSE given), nothing is recorded and the program has no licence options.
function(sc_program_info target)
    cmake_parse_arguments(ARG "" "LICENSE;URL;TEXT_URL" "" ${ARGN})
    sc_license_file(license_file)
    set(name "${ARG_LICENSE}")
    if (NOT name AND license_file)
        sc_license_name(name "${license_file}")
    endif ()
    if (NOT name)
        return()
    endif ()

    # Where the project lives: its HOMEPAGE_URL, which deploys keep (they copy the source without
    # .git), or else its git remote.
    set(repository "${PROJECT_HOMEPAGE_URL}")
    if (NOT repository)
        find_package(Git QUIET)
        if (GIT_FOUND)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" remote get-url origin
                    OUTPUT_VARIABLE repository OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif ()
    endif ()
    string(REGEX REPLACE "^git@github\\.com:" "https://github.com/" repository "${repository}")
    string(REGEX REPLACE "\\.git$" "" repository "${repository}")
    set(url "${ARG_URL}")
    set(text_url "${ARG_TEXT_URL}")
    if (license_file AND repository MATCHES "^https://github\\.com/([^/]+/[^/]+)$")
        set(slug "${CMAKE_MATCH_1}")
        get_filename_component(license_name "${license_file}" NAME)
        set(ref main)
        if (SC_VERSION AND NOT SC_VERSION STREQUAL "0.0.0")
            set(ref "v${SC_VERSION}")
        endif ()
        if (NOT url)
            set(url "${repository}/blob/${ref}/${license_name}")
        endif ()
        if (NOT text_url)
            set(text_url "https://raw.githubusercontent.com/${slug}/${ref}/${license_name}")
        endif ()
    elseif (NOT url AND name MATCHES "^[A-Za-z0-9.-]+$")
        set(url "https://spdx.org/licenses/${name}.html")
    endif ()

    # The copyright: the first year and owner in the project's NOTICE ("Copyright 2026 Roelof
    # Rossouw"), on to the year it's built; none without a NOTICE.
    set(copyright "")
    if (EXISTS "${PROJECT_SOURCE_DIR}/NOTICE")
        file(STRINGS "${PROJECT_SOURCE_DIR}/NOTICE" notice_lines REGEX "^Copyright [0-9][0-9][0-9][0-9]")
        list(GET notice_lines 0 notice_line)
        if (notice_line MATCHES "^Copyright ([0-9][0-9][0-9][0-9])(-[0-9][0-9][0-9][0-9])? (.+)$")
            set(since "${CMAKE_MATCH_1}")
            set(owner "${CMAKE_MATCH_3}")
            string(TIMESTAMP now "%Y")
            set(years "${since}")
            if (now GREATER since)
                set(years "${since}-${now}")
            endif ()
            set(copyright "Copyright ${years} ${owner}")
        endif ()
    endif ()

    sc_package_name(package)
    set(files "\"/usr/share/doc/${package}/copyright\"")
    if (NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        string(APPEND files ", \"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/doc/${package}/copyright\"")
    endif ()
    if (license_file)
        string(APPEND files ", \"${license_file}\"")
    endif ()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/sc_program_info/${target}")
    file(CONFIGURE OUTPUT "${directory}/sc_program_info.h" CONTENT [=[
// Generated by sc_program_info() (SimplyCppFunctions.cmake): the program's licence, for sc::params.
#pragma once

#define SC_PROGRAM_LICENSE "@name@"
#define SC_PROGRAM_LICENSE_URL "@url@"
#define SC_PROGRAM_LICENSE_TEXT_URL "@text_url@"
static const char *const sc_program_license_files[] = {@files@, nullptr};
#define SC_PROGRAM_COPYRIGHT "@copyright@"
]=] @ONLY)
    target_include_directories(${target} PRIVATE "${directory}")
    target_compile_definitions(${target} PRIVATE "SC_PROGRAM_INFO_HEADER=\"sc_program_info.h\"")
endfunction()

# Sync README.md once, after the whole CMakeLists.txt has been read.
function(sc_sync_readme_later)
    get_property(scheduled DIRECTORY PROPERTY SC_README_SYNC_SCHEDULED)
    if (NOT scheduled)
        set_property(DIRECTORY PROPERTY SC_README_SYNC_SCHEDULED TRUE)
        cmake_language(DEFER CALL sc_sync_readme)
    endif ()
endfunction()

# sc_sync_readme([<readme>])
# Replace each block between
#   <!-- sc-example: <file> -->  and  <!-- /sc-example -->
# with a cpp code block holding <file> (relative to the module root). When the file has
# `// [readme]` and `// [/readme]` lines, only the lines between them are used.
function(sc_sync_readme)
    set(readme "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    if (ARGC GREATER 0)
        set(readme "${ARGV0}")
    endif ()
    if (NOT EXISTS "${readme}")
        return()
    endif ()

    set(open_marker "<!-- sc-example:")
    set(close_marker "<!-- /sc-example -->")
    string(LENGTH "${close_marker}" close_length)
    set(fence "```")

    file(READ "${readme}" original)
    set(rest "${original}")
    set(result "")
    while (TRUE)
        string(FIND "${rest}" "${open_marker}" start)
        if (start EQUAL -1)
            string(APPEND result "${rest}")
            break()
        endif ()
        string(SUBSTRING "${rest}" 0 ${start} before)
        string(APPEND result "${before}")
        string(SUBSTRING "${rest}" ${start} -1 rest)

        string(FIND "${rest}" "-->" marker_end)
        string(FIND "${rest}" "${close_marker}" block_end)
        if (marker_end EQUAL -1 OR block_end EQUAL -1)
            message(WARNING "${readme}: unterminated ${open_marker} block, left as is")
            string(APPEND result "${rest}")
            break()
        endif ()
        math(EXPR marker_end "${marker_end} + 3")
        string(SUBSTRING "${rest}" 0 ${marker_end} marker)
        string(REGEX REPLACE "^${open_marker} *([^ ]+) *-->$" "\\1" source "${marker}")

        sc_readme_snippet("${CMAKE_CURRENT_SOURCE_DIR}/${source}" snippet)
        if (snippet STREQUAL "")
            message(WARNING "${readme}: ${source} not found or empty, block left as is")
            math(EXPR keep "${block_end} + ${close_length}")
            string(SUBSTRING "${rest}" 0 ${keep} unchanged)
            string(APPEND result "${unchanged}")
        else ()
            string(APPEND result "${marker}\n${fence}cpp\n${snippet}\n${fence}\n${close_marker}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif ()
        math(EXPR after "${block_end} + ${close_length}")
        string(SUBSTRING "${rest}" ${after} -1 rest)
    endwhile ()

    if (NOT result STREQUAL original)
        file(WRITE "${readme}" "${result}")
        message(STATUS "Updated examples in ${readme}")
    endif ()
endfunction()

# The README part of a source file, dedented, without trailing newlines.
function(sc_readme_snippet file output)
    set(${output} "" PARENT_SCOPE)
    if (NOT EXISTS "${file}")
        return()
    endif ()
    file(READ "${file}" text)

    # Keep list separators and brackets out of the regex/list handling below.
    string(ASCII 1 semicolon)
    string(ASCII 2 open_bracket)
    string(ASCII 3 close_bracket)
    string(REPLACE ";" "${semicolon}" text "${text}")
    string(REPLACE "[" "${open_bracket}" text "${text}")
    string(REPLACE "]" "${close_bracket}" text "${text}")
    string(REPLACE "\r\n" "\n" text "${text}")

    set(begin_marker "// ${open_bracket}readme${close_bracket}")
    set(end_marker "// ${open_bracket}/readme${close_bracket}")
    string(FIND "${text}" "${begin_marker}" begin)
    string(FIND "${text}" "${end_marker}" end)
    if (NOT begin EQUAL -1 AND end GREATER begin)
        string(SUBSTRING "${text}" ${begin} -1 text)
        string(FIND "${text}" "\n" first_line_end)
        string(FIND "${text}" "${end_marker}" end)
        math(EXPR length "${end} - ${first_line_end} - 1")
        math(EXPR first_line_end "${first_line_end} + 1")
        string(SUBSTRING "${text}" ${first_line_end} ${length} text)
    endif ()

    # Drop the indentation shared by every non-blank line.
    string(REGEX REPLACE "\n[ \t]+\n" "\n\n" text "\n${text}")
    string(REGEX MATCHALL "\n *[^ \n]" indents "${text}")
    set(indent -1)
    foreach (line_start ${indents})
        string(LENGTH "${line_start}" width)
        math(EXPR width "${width} - 2")
        if (indent EQUAL -1 OR width LESS indent)
            set(indent ${width})
        endif ()
    endforeach ()
    if (indent GREATER 0)
        string(REPEAT " " ${indent} prefix)
        string(REPLACE "\n${prefix}" "\n" text "${text}")
    endif ()
    string(REGEX REPLACE "^\n+" "" text "${text}")
    string(REGEX REPLACE "[ \t\n]+$" "" text "${text}")

    string(REPLACE "${semicolon}" ";" text "${text}")
    string(REPLACE "${open_bracket}" "[" text "${text}")
    string(REPLACE "${close_bracket}" "]" text "${text}")
    set(${output} "${text}" PARENT_SCOPE)
endfunction()
