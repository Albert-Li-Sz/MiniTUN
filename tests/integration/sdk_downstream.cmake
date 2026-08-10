if(NOT DEFINED MINITUN_BUILD_DIR OR NOT DEFINED MINITUN_SOURCE_DIR)
    message(FATAL_ERROR "MINITUN_BUILD_DIR and MINITUN_SOURCE_DIR are required")
endif()

set(prefix "${MINITUN_BUILD_DIR}/sdk-downstream-prefix")
set(build "${MINITUN_BUILD_DIR}/sdk-downstream-build")
file(REMOVE_RECURSE "${prefix}" "${build}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MINITUN_BUILD_DIR}" --prefix "${prefix}"
            --component ClientLibrary
    RESULT_VARIABLE install_library_result
)
if(NOT install_library_result EQUAL 0)
    message(FATAL_ERROR "failed to install the SDK runtime")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MINITUN_BUILD_DIR}" --prefix "${prefix}"
            --component ClientDevelopment
    RESULT_VARIABLE install_development_result
)
if(NOT install_development_result EQUAL 0)
    message(FATAL_ERROR "failed to install the SDK development files")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MINITUN_SOURCE_DIR}/tests/sdk/downstream" -B "${build}"
            "-DCMAKE_PREFIX_PATH=${prefix}"
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "downstream find_package configuration failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "downstream SDK build failed")
endif()

message(STATUS "downstream MiniTun SDK package test passed")
