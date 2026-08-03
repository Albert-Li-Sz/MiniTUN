find_package(Git QUIET)

set(MINITUN_GIT_COMMIT "unknown")
set(MINITUN_GIT_TAG "")
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    # A normal build must refresh version metadata after a commit, branch move,
    # or tag change without requiring the operator to rerun CMake manually.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE MINITUN_GIT_DIR_RESULT
        OUTPUT_VARIABLE MINITUN_GIT_DIR
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(MINITUN_GIT_DIR_RESULT EQUAL 0 AND IS_DIRECTORY "${MINITUN_GIT_DIR}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-common-dir
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE MINITUN_GIT_COMMON_DIR_RESULT
            OUTPUT_VARIABLE MINITUN_GIT_COMMON_DIR
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT MINITUN_GIT_COMMON_DIR_RESULT EQUAL 0 OR
           NOT IS_DIRECTORY "${MINITUN_GIT_COMMON_DIR}")
            set(MINITUN_GIT_COMMON_DIR "${MINITUN_GIT_DIR}")
        endif()

        foreach(git_metadata IN ITEMS HEAD packed-refs)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute
                        --git-path "${git_metadata}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                RESULT_VARIABLE MINITUN_GIT_PATH_RESULT
                OUTPUT_VARIABLE MINITUN_GIT_PATH
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(MINITUN_GIT_PATH_RESULT EQUAL 0 AND EXISTS "${MINITUN_GIT_PATH}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${MINITUN_GIT_PATH}"
                )
            endif()
        endforeach()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" symbolic-ref --quiet HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE MINITUN_GIT_REF_RESULT
            OUTPUT_VARIABLE MINITUN_GIT_REF
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(MINITUN_GIT_REF_RESULT EQUAL 0 AND NOT MINITUN_GIT_REF STREQUAL "")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute
                        --git-path "${MINITUN_GIT_REF}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                RESULT_VARIABLE MINITUN_GIT_REF_PATH_RESULT
                OUTPUT_VARIABLE MINITUN_GIT_REF_PATH
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(MINITUN_GIT_REF_PATH_RESULT EQUAL 0 AND EXISTS "${MINITUN_GIT_REF_PATH}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${MINITUN_GIT_REF_PATH}"
                )
            endif()
        endif()

        file(GLOB_RECURSE MINITUN_GIT_TAG_REFS CONFIGURE_DEPENDS
            "${MINITUN_GIT_COMMON_DIR}/refs/tags/*"
        )
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE MINITUN_GIT_RESULT
        OUTPUT_VARIABLE MINITUN_GIT_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(MINITUN_GIT_RESULT EQUAL 0 AND NOT MINITUN_GIT_OUTPUT STREQUAL "")
        set(MINITUN_GIT_COMMIT "${MINITUN_GIT_OUTPUT}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v[0-9]*" HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE MINITUN_TAG_RESULT
        OUTPUT_VARIABLE MINITUN_TAG_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(MINITUN_TAG_RESULT EQUAL 0 AND MINITUN_TAG_OUTPUT MATCHES "^v.+")
        string(SUBSTRING "${MINITUN_TAG_OUTPUT}" 1 -1 MINITUN_GIT_TAG)
    endif()
endif()

if(NOT "${MINITUN_PACKAGE_VERSION}" STREQUAL "${PROJECT_VERSION}")
    set(MINITUN_VERSION_STRING "${MINITUN_PACKAGE_VERSION}")
elseif(MINITUN_GIT_TAG STREQUAL PROJECT_VERSION)
    set(MINITUN_VERSION_STRING "${MINITUN_GIT_TAG}")
elseif(NOT MINITUN_GIT_COMMIT STREQUAL "unknown")
    set(MINITUN_VERSION_STRING "${PROJECT_VERSION}-dev+g${MINITUN_GIT_COMMIT}")
else()
    set(MINITUN_VERSION_STRING "${PROJECT_VERSION}")
endif()

function(minitun_apply_version_definitions target)
    target_compile_definitions("${target}"
        PRIVATE
            MINITUN_VERSION="${MINITUN_VERSION_STRING}"
            MINITUN_GIT_COMMIT="${MINITUN_GIT_COMMIT}"
            MINITUN_BUILD_TYPE="$<CONFIG>"
            MINITUN_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            MINITUN_PROTOCOL_VERSION=1
    )
endfunction()
