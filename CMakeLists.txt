cmake_minimum_required(VERSION 3.22)

project(
    MiniTun
    VERSION 1.0.0
    DESCRIPTION "A minimal-footprint, self-hosted multi-transport reverse tunnel"
    HOMEPAGE_URL "https://github.com/Albert-Li-Sz/MiniTUN"
    LANGUAGES C CXX
)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(MINITUN_USE_SYSTEM_DEPS "Use distribution-provided dependencies" ON)
option(MINITUN_BUILD_TESTS "Build the MiniTun test suite" ON)
option(MINITUN_BUILD_FUZZERS "Build fuzz targets" OFF)
option(MINITUN_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINITUN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINITUN_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MINITUN_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(MINITUN_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(MINITUN_ENABLE_COVERAGE "Instrument project targets for gcov-compatible coverage" OFF)
option(MINITUN_ENABLE_FAULT_INJECTION "Enable test-only crash failpoints" OFF)
option(MINITUN_BUILD_PACKAGES "Enable package-generation support" OFF)
set(MINITUN_PACKAGE_VERSION "" CACHE STRING
    "Optional release or CI development package version override"
)

# Static libraries linked into libminitun-client must use position-independent
# code so TLS models and relocations stay valid in the shared object across
# GCC, Clang, and system Asio variants.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MINITUN_PACKAGE_VERSION STREQUAL "")
    set(MINITUN_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()

if(NOT MINITUN_PACKAGE_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-rc\\.[0-9]+|_pre[0-9]+~[0-9a-f]+)?$")
    message(FATAL_ERROR
        "MINITUN_PACKAGE_VERSION must be a release, release candidate, or CI development version"
    )
endif()

string(REGEX REPLACE "(-rc\\.[0-9]+|_pre[0-9]+~[0-9a-f]+)$" ""
    MINITUN_PACKAGE_BASE_VERSION "${MINITUN_PACKAGE_VERSION}"
)
if(NOT "${MINITUN_PACKAGE_BASE_VERSION}" STREQUAL "${PROJECT_VERSION}")
    message(FATAL_ERROR
        "MINITUN_PACKAGE_VERSION base version must match PROJECT_VERSION (${PROJECT_VERSION})"
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Dependencies)
include(Sanitizers)
include(Version)

minitun_setup_dependencies()

if(MINITUN_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "MINITUN_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    if(CMAKE_BUILD_TYPE MATCHES "Release|MinSizeRel")
        message(FATAL_ERROR "MINITUN_ENABLE_COVERAGE requires a non-release build")
    endif()
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

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
    src/common/failpoint.cpp
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
if(MINITUN_ENABLE_FAULT_INJECTION)
    target_compile_definitions(minitun_common PRIVATE MINITUN_ENABLE_FAULT_INJECTION)
endif()
minitun_apply_sanitizers(minitun_common)
minitun_apply_version_definitions(minitun_common)
minitun_apply_lto(minitun_common)

add_library(minitun_protocol STATIC
    src/protocol/auth.cpp
    src/protocol/codec.cpp
    src/protocol/datagram.cpp
    src/protocol/frame.cpp
    src/protocol/messages.cpp
    src/protocol/p2p.cpp
    src/protocol/relay.cpp
    src/protocol/socks5.cpp
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

add_library(minitun_remote_protocol SHARED src/sdk/remote_protocol.cpp)
add_library(MiniTun::RemoteProtocol ALIAS minitun_remote_protocol)
set_target_properties(minitun_remote_protocol PROPERTIES
    OUTPUT_NAME minitun-remote-protocol
    EXPORT_NAME RemoteProtocol
    VERSION "${PROJECT_VERSION}"
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_definitions(minitun_remote_protocol PRIVATE MINITUN_REMOTE_PROTOCOL_BUILD)
target_include_directories(minitun_remote_protocol
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(minitun_remote_protocol PUBLIC cxx_std_20)
target_link_libraries(minitun_remote_protocol
    PRIVATE MiniTun::protocol minitun_project_warnings
)
if(APPLE)
    target_link_options(minitun_remote_protocol PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-remote-protocol-1.apple.exports"
    )
    set_property(TARGET minitun_remote_protocol APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-remote-protocol-1.apple.exports"
    )
elseif(UNIX)
    target_link_options(minitun_remote_protocol PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-remote-protocol-1.map"
    )
    set_property(TARGET minitun_remote_protocol APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-remote-protocol-1.map"
    )
endif()
minitun_apply_sanitizers(minitun_remote_protocol)
minitun_apply_lto(minitun_remote_protocol)

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

add_library(minitun_admin STATIC src/admin/server.cpp)
add_library(MiniTun::admin ALIAS minitun_admin)
target_include_directories(minitun_admin
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_admin
    PUBLIC MiniTun::common MiniTun::asio
    PRIVATE minitun_project_warnings
)
minitun_apply_sanitizers(minitun_admin)
minitun_apply_lto(minitun_admin)

add_library(minitun_client SHARED src/sdk/client.cpp)
add_library(MiniTun::Client ALIAS minitun_client)
set_target_properties(minitun_client PROPERTIES
    OUTPUT_NAME minitun-client
    EXPORT_NAME Client
    VERSION "${PROJECT_VERSION}"
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_definitions(minitun_client PRIVATE MINITUN_CLIENT_BUILD)
target_include_directories(minitun_client
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(minitun_client
    PRIVATE
        MiniTun::ipc
        minitun_project_warnings
)
if(APPLE)
    target_link_options(minitun_client PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-client-1.apple.exports"
    )
elseif(UNIX)
    target_link_options(minitun_client PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/abi/minitun-client-1.map"
    )
endif()
minitun_apply_sanitizers(minitun_client)
minitun_apply_lto(minitun_client)

add_library(minitun_daemon STATIC
    src/daemon/control_service.cpp
    src/daemon/declarative_config.cpp
    src/daemon/reconnect_backoff.cpp
    src/daemon/server_manager.cpp
    src/daemon/tunnel_reconciler.cpp
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
    src/server/accept_recovery.cpp
    src/server/client_policy.cpp
    src/server/connection_quota.cpp
    src/server/policy_management.cpp
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
        MiniTun::admin
        nlohmann_json::nlohmann_json
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
minitun_add_program(minitun-p2p apps/minitun_p2p_main.cpp MiniTun::protocol)
target_link_libraries(minitund PRIVATE MiniTun::admin)
target_link_libraries(minitun-server PRIVATE MiniTun::admin)

set(MINITUN_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    CACHE STRING "Installation directory for systemd service units"
)
set(MINITUN_SYSUSERS_DIR "${CMAKE_INSTALL_LIBDIR}/sysusers.d"
    CACHE STRING "Installation directory for systemd-sysusers definitions"
)

install(
    TARGETS minitun
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Client
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MiniTunConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MiniTunConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MiniTun"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MiniTunConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/pkgconfig/minitun-client.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/minitun-client.pc"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/pkgconfig/minitun-remote-protocol.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/minitun-remote-protocol.pc"
    @ONLY
)
install(
    EXPORT MiniTunTargets
    FILE MiniTunTargets.cmake
    NAMESPACE MiniTun::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MiniTun"
    COMPONENT ClientDevelopment
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/MiniTunConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MiniTunConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MiniTun"
    COMPONENT ClientDevelopment
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/minitun-client.pc"
        "${CMAKE_CURRENT_BINARY_DIR}/minitun-remote-protocol.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT ClientDevelopment
)
install(
    TARGETS minitund
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/minitun"
    COMPONENT Client
)
install(
    TARGETS minitun-server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Server
)
install(
    TARGETS minitun-p2p
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Client
)
install(
    TARGETS minitun_client
    EXPORT MiniTunTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT ClientLibrary
            NAMELINK_COMPONENT ClientDevelopment
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ClientLibrary
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ClientDevelopment
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    TARGETS minitun_remote_protocol
    EXPORT MiniTunTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT ClientLibrary
            NAMELINK_COMPONENT ClientDevelopment
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ClientLibrary
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ClientDevelopment
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    FILES packaging/systemd/minitund.service
    DESTINATION "${MINITUN_SYSTEMD_UNIT_DIR}"
    COMPONENT Client
)
install(
    FILES packaging/systemd/minitun-server.service
    DESTINATION "${MINITUN_SYSTEMD_UNIT_DIR}"
    COMPONENT Server
)
install(
    FILES packaging/sysusers/minitun.conf
    DESTINATION "${MINITUN_SYSUSERS_DIR}"
    COMPONENT Client
)
install(
    FILES packaging/systemd/minitund-backup.service
          packaging/systemd/minitund-backup.timer
    DESTINATION "${MINITUN_SYSTEMD_UNIT_DIR}"
    COMPONENT Client
)
install(
    FILES packaging/openrc/minitund
    DESTINATION "${CMAKE_INSTALL_DATADIR}/minitun/openrc"
    COMPONENT Client
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
install(
    FILES packaging/openrc/minitun-server
    DESTINATION "${CMAKE_INSTALL_DATADIR}/minitun/openrc"
    COMPONENT Server
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
install(
    FILES packaging/s6/minitund/run
    DESTINATION "${CMAKE_INSTALL_DATADIR}/minitun/s6/minitund"
    COMPONENT Client
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
install(
    FILES packaging/s6/minitun-server/run
    DESTINATION "${CMAKE_INSTALL_DATADIR}/minitun/s6/minitun-server"
    COMPONENT Server
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
install(
    FILES contrib/docker-compose.yml
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/client/examples"
    COMPONENT Client
)
install(
    FILES packaging/sysusers/minitun-server.conf
    DESTINATION "${MINITUN_SYSUSERS_DIR}"
    COMPONENT Server
)

install(
    FILES packaging/man/minitun.1
          packaging/man/minitun-p2p.1
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
    COMPONENT Client
)
install(
    FILES packaging/man/minitund.8
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
    COMPONENT Client
)
install(
    FILES packaging/man/minitun-server.8
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
    COMPONENT Server
)

install(
    FILES README.md LICENSE
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/client"
    COMPONENT Client
)
install(
    FILES README.md LICENSE packaging/config/minitun-server/README
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/server"
    COMPONENT Server
)
install(
    FILES packaging/config/minitun-server/README
          packaging/config/minitun-server/clients.json.example
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/minitun-server"
    COMPONENT Server
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)
install(
    DIRECTORY include/minitun
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT Development
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)
install(
    FILES include/minitun/client.h include/minitun/client.hpp include/minitun/remote_protocol.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/minitun"
    COMPONENT ClientDevelopment
)
install(
    DIRECTORY include/minitun/common include/minitun/protocol
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/minitun"
    COMPONENT ClientDevelopment
    FILES_MATCHING PATTERN "*.hpp"
)

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
    include(Packaging)
endif()
