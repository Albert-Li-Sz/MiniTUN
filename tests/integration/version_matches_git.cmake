if(NOT DEFINED MINITUN_EXECUTABLE OR NOT DEFINED MINITUN_SOURCE_DIR OR
   NOT DEFINED MINITUN_GIT_EXECUTABLE)
    message(FATAL_ERROR "version test arguments are incomplete")
endif()

execute_process(
    COMMAND "${MINITUN_GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${MINITUN_SOURCE_DIR}"
    RESULT_VARIABLE git_status
    OUTPUT_VARIABLE expected_commit
    ERROR_VARIABLE git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT git_status EQUAL 0 OR expected_commit STREQUAL "")
    message(FATAL_ERROR "failed to inspect Git HEAD: ${git_error}")
endif()

execute_process(
    COMMAND "${MINITUN_EXECUTABLE}" version
    RESULT_VARIABLE version_status
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_status EQUAL 0)
    message(FATAL_ERROR "version command failed: ${version_error}")
endif()

string(REGEX MATCH "git commit: ([0-9a-f]+)" commit_line "${version_output}")
if(NOT CMAKE_MATCH_1 STREQUAL expected_commit)
    message(FATAL_ERROR
        "binary Git commit '${CMAKE_MATCH_1}' does not match HEAD '${expected_commit}'"
    )
endif()

message(STATUS "binary version matches Git HEAD ${expected_commit}")
