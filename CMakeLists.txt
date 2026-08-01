cmake_minimum_required(VERSION 3.22)

project(
    MiniTun
    VERSION 0.1.0
    DESCRIPTION "A small, high-performance TCP reverse-tunnelling system"
    HOMEPAGE_URL "https://github.com/LMTINSUZHOU/MiniTUN"
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)

option(MINITUN_USE_SYSTEM_DEPS "Use distribution-provided dependencies" ON)
option(MINITUN_BUILD_TESTS "Build the MiniTun test suite" ON)
option(MINITUN_BUILD_FUZZERS "Build fuzz targets" OFF)
option(MINITUN_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINITUN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINITUN_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MINITUN_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(MINITUN_BUILD_PACKAGES "Enable package-generation support" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Dependencies)
include(Sanitizers)
include(Version)

minitun_setup_dependencies()

if(MINITUN_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT minitun_lto_supported OUTPUT minitun_lto_error)
    if(NOT minitun_lto_supported)
        message(FATAL_ERROR "LTO is unavailable: ${minitun_lto_error}")
    endif()
endif()

function(minitun_apply_lto target)
    if(MINITUN_ENABLE_LTO)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

add_library(minitun_common STATIC
    src/common/endpoint.cpp
    src/common/error.cpp
    src/common/id.cpp
    src/common/logging.cpp
    src/common/port_range.cpp
    src/common/secure_string.cpp
    src/common/time.cpp
    src/common/version.cpp
)
add_library(MiniTun::common ALIAS minitun_common)

target_include_directories(minitun_common
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_common
    PUBLIC
        spdlog::spdlog
    PRIVATE
        MiniTun::dependencies
        Threads::Threads
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_common)
minitun_apply_version_definitions(minitun_common)
minitun_apply_lto(minitun_common)

add_library(minitun_protocol STATIC
    src/protocol/auth.cpp
    src/protocol/codec.cpp
    src/protocol/frame.cpp
    src/protocol/messages.cpp
    src/protocol/relay.cpp
    src/protocol/state_machine.cpp
    src/protocol/tls.cpp
)
add_library(MiniTun::protocol ALIAS minitun_protocol)

target_include_directories(minitun_protocol
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_protocol
    PUBLIC
        MiniTun::common
        MiniTun::asio
        OpenSSL::SSL
        OpenSSL::Crypto
    PRIVATE
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_protocol)
minitun_apply_lto(minitun_protocol)

add_library(minitun_storage STATIC
    src/storage/credential_store.cpp
    src/storage/database.cpp
    src/storage/file_security.cpp
    src/storage/models.cpp
    src/storage/server_repository.cpp
    src/storage/sqlite_internal.cpp
    src/storage/state_repository.cpp
    src/storage/tunnel_repository.cpp
)
add_library(MiniTun::storage ALIAS minitun_storage)

target_include_directories(minitun_storage
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_storage
    PUBLIC
        MiniTun::common
    PRIVATE
        MiniTun::sqlite
        Threads::Threads
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_storage)
minitun_apply_lto(minitun_storage)

add_library(minitun_ipc STATIC
    src/ipc/dispatcher.cpp
    src/ipc/frame.cpp
    src/ipc/local_client.cpp
    src/ipc/local_server.cpp
    src/ipc/protocol.cpp
)
add_library(MiniTun::ipc ALIAS minitun_ipc)

target_include_directories(minitun_ipc
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_ipc
    PUBLIC
        MiniTun::common
        MiniTun::asio
        nlohmann_json::nlohmann_json
    PRIVATE
        Threads::Threads
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_ipc)
minitun_apply_lto(minitun_ipc)

add_library(minitun_daemon STATIC
    src/daemon/control_service.cpp
    src/daemon/reconnect_backoff.cpp
    src/daemon/server_manager.cpp
    src/daemon/worker_pool.cpp
)
add_library(MiniTun::daemon ALIAS minitun_daemon)

target_include_directories(minitun_daemon
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_daemon
    PUBLIC
        MiniTun::ipc
        MiniTun::protocol
        MiniTun::storage
    PRIVATE
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_daemon)
minitun_apply_lto(minitun_daemon)

add_library(minitun_server_core STATIC
    src/server/connection_quota.cpp
    src/server/server.cpp
    src/server/session_registry.cpp
    src/server/tunnel_registry.cpp
    src/server/worker_pool.cpp
)
add_library(MiniTun::server ALIAS minitun_server_core)

target_include_directories(minitun_server_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_server_core
    PUBLIC
        MiniTun::protocol
    PRIVATE
        Threads::Threads
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_server_core)
minitun_apply_lto(minitun_server_core)

function(minitun_add_program target source core_library)
    add_executable("${target}" "${source}")
    target_link_libraries("${target}"
        PRIVATE
            "${core_library}"
            CLI11::CLI11
            minitun_project_warnings
    )
    minitun_apply_sanitizers("${target}")
    minitun_apply_version_definitions("${target}")
    minitun_apply_lto("${target}")
endfunction()

minitun_add_program(minitun apps/minitun_main.cpp MiniTun::ipc)
minitun_add_program(minitund apps/minitund_main.cpp MiniTun::daemon)
minitun_add_program(minitun-server apps/minitun_server_main.cpp MiniTun::server)

if(MINITUN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MINITUN_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MINITUN_BUILD_FUZZERS requires Clang with libFuzzer")
    endif()

    foreach(target minitun_common minitun_protocol minitun_ipc)
        target_compile_options("${target}" PRIVATE -fsanitize=fuzzer-no-link)
    endforeach()

    function(minitun_add_fuzzer target source)
        add_executable("${target}" "${source}")
        target_link_libraries("${target}"
            PRIVATE
                ${ARGN}
                minitun_project_warnings
        )
        target_compile_options("${target}" PRIVATE -fsanitize=fuzzer)
        target_link_options("${target}" PRIVATE -fsanitize=fuzzer)
        minitun_apply_sanitizers("${target}")
    endfunction()

    minitun_add_fuzzer(minitun_remote_frame_fuzz fuzz/remote_frame_fuzz.cpp MiniTun::protocol)
    minitun_add_fuzzer(minitun_ipc_frame_fuzz fuzz/ipc_frame_fuzz.cpp MiniTun::ipc)
    minitun_add_fuzzer(minitun_ipc_json_fuzz fuzz/ipc_json_fuzz.cpp MiniTun::ipc)
    minitun_add_fuzzer(minitun_endpoint_fuzz fuzz/endpoint_fuzz.cpp MiniTun::common)
    minitun_add_fuzzer(minitun_port_range_fuzz fuzz/port_range_fuzz.cpp MiniTun::common)

    add_custom_target(minitun_fuzzers
        DEPENDS
            minitun_remote_frame_fuzz
            minitun_ipc_frame_fuzz
            minitun_ipc_json_fuzz
            minitun_endpoint_fuzz
            minitun_port_range_fuzz
    )
endif()

if(MINITUN_BUILD_PACKAGES)
    message(STATUS "Package generation will be introduced in packaging phase 14")
endif()
