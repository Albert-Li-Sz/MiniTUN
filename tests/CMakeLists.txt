add_executable(minitun_unit_tests
    unit/accept_recovery_test.cpp
    unit/admin_server_test.cpp
    unit/common_test.cpp
    unit/client_policy_test.cpp
    unit/connection_quota_test.cpp
    unit/credential_store_test.cpp
    unit/credential_keys_test.cpp
    unit/daemon_control_service_test.cpp
    unit/endpoint_test.cpp
    unit/id_test.cpp
    unit/ipc_dispatcher_test.cpp
    unit/ipc_frame_test.cpp
    unit/ipc_protocol_test.cpp
    unit/ipc_transport_test.cpp
    unit/logging_test.cpp
    unit/port_range_test.cpp
    unit/remote_codec_test.cpp
    unit/remote_auth_test.cpp
    unit/remote_frame_test.cpp
    unit/remote_messages_test.cpp
    unit/relay_test.cpp
    unit/remote_state_machine_test.cpp
    unit/reconnect_backoff_test.cpp
    unit/sdk_client_test.cpp
    unit/server_manager_test.cpp
    unit/server_session_registry_test.cpp
    unit/secure_string_test.cpp
    unit/sqlite_internal_test.cpp
    unit/state_recovery_test.cpp
    unit/storage_database_test.cpp
    unit/storage_diagnostics_test.cpp
    unit/storage_models_test.cpp
    unit/storage_repository_test.cpp
    unit/time_test.cpp
    unit/tls_test.cpp
    unit/tunnel_registry_test.cpp
    unit/tunnel_reconciler_test.cpp
    unit/worker_pool_test.cpp
)

target_include_directories(minitun_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/storage")

target_link_libraries(minitun_unit_tests
    PRIVATE
        MiniTun::daemon
        MiniTun::admin
        MiniTun::Client
        MiniTun::server
        MiniTun::storage
        MiniTun::ipc
        MiniTun::sqlite
        GTest::gtest_main
        nlohmann_json::nlohmann_json
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_unit_tests)
minitun_apply_lto(minitun_unit_tests)

add_executable(minitun_tls_auth_client helpers/tls_auth_client.cpp)
target_link_libraries(minitun_tls_auth_client
    PRIVATE
        MiniTun::protocol
        CLI11::CLI11
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_tls_auth_client)
minitun_apply_lto(minitun_tls_auth_client)

add_executable(minitun_c_sdk_smoke sdk/c_client_smoke.c)
target_link_libraries(minitun_c_sdk_smoke PRIVATE MiniTun::Client)
set_target_properties(minitun_c_sdk_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
minitun_apply_sanitizers(minitun_c_sdk_smoke)

add_executable(minitun_cpp_sdk_smoke sdk/cpp_client_smoke.cpp)
target_link_libraries(minitun_cpp_sdk_smoke PRIVATE MiniTun::Client minitun_project_warnings)
set_target_properties(minitun_cpp_sdk_smoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
minitun_apply_sanitizers(minitun_cpp_sdk_smoke)

add_test(NAME sdk.c11-smoke COMMAND minitun_c_sdk_smoke)
set_tests_properties(sdk.c11-smoke PROPERTIES LABELS "sdk;smoke")
add_test(NAME sdk.cpp20-smoke COMMAND minitun_cpp_sdk_smoke)
set_tests_properties(sdk.cpp20-smoke PROPERTIES LABELS "sdk;smoke")
add_test(NAME sdk.find-package
    COMMAND "${CMAKE_COMMAND}"
        "-DMINITUN_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DMINITUN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/sdk_downstream.cmake"
)
set(minitun_sdk_sanitizer_flags "")
if(MINITUN_ENABLE_ASAN)
    list(APPEND minitun_sdk_sanitizer_flags "address")
endif()
if(MINITUN_ENABLE_UBSAN)
    list(APPEND minitun_sdk_sanitizer_flags "undefined")
endif()
if(MINITUN_ENABLE_TSAN)
    list(APPEND minitun_sdk_sanitizer_flags "thread")
endif()
if(minitun_sdk_sanitizer_flags)
    list(JOIN minitun_sdk_sanitizer_flags "," minitun_sdk_sanitizer_joined)
    set(minitun_sdk_sanitizer_compile_flags
        "-fsanitize=${minitun_sdk_sanitizer_joined} -fno-omit-frame-pointer")
    set_tests_properties(sdk.find-package PROPERTIES
        ENVIRONMENT "MINITUN_SDK_C_FLAGS=${minitun_sdk_sanitizer_compile_flags};MINITUN_SDK_CXX_FLAGS=${minitun_sdk_sanitizer_compile_flags}"
    )
endif()
set_tests_properties(sdk.find-package PROPERTIES LABELS "sdk;install" TIMEOUT 60)
add_test(NAME sdk.abi-baseline
    COMMAND "${CMAKE_COMMAND}"
        "-DMINITUN_LIBRARY=$<TARGET_FILE:minitun_client>"
        "-DMINITUN_NM=${CMAKE_NM}"
        "-DMINITUN_BASELINE=${CMAKE_SOURCE_DIR}/abi/minitun-client-1.symbols"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/sdk_abi.cmake"
)
set_tests_properties(sdk.abi-baseline PROPERTIES LABELS "sdk;abi")

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME release.evidence-verifier
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/release/test_verify_release_evidence.py"
    )
    set_tests_properties(release.evidence-verifier PROPERTIES
        LABELS "release;performance;security"
        ENVIRONMENT "MINITUN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME security.sarif-gate
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/release/test_check_sarif.py"
    )
    set_tests_properties(security.sarif-gate PROPERTIES
        LABELS "security;codeql;release"
        ENVIRONMENT "MINITUN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    )
endif()

include(GoogleTest)
gtest_discover_tests(minitun_unit_tests
    TEST_PREFIX "unit."
    PROPERTIES LABELS "unit"
)

add_test(NAME smoke.minitun-version
    COMMAND $<TARGET_FILE:minitun> version
)
set_tests_properties(smoke.minitun-version PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "protocol version: 2"
)

if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    add_test(NAME smoke.version-matches-git-head
        COMMAND "${CMAKE_COMMAND}"
            "-DMINITUN_EXECUTABLE=$<TARGET_FILE:minitun>"
            "-DMINITUN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DMINITUN_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/version_matches_git.cmake"
    )
    set_tests_properties(smoke.version-matches-git-head PROPERTIES LABELS "smoke;version")
endif()

add_test(NAME smoke.minitund-help
    COMMAND $<TARGET_FILE:minitund> --help
)
set_tests_properties(smoke.minitund-help PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "MiniTun client daemon"
)

add_test(NAME smoke.server-help
    COMMAND $<TARGET_FILE:minitun-server> --help
)
set_tests_properties(smoke.server-help PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "MiniTun public server"
)

if(UNIX)
    if(MINITUN_ENABLE_FAULT_INJECTION)
        foreach(failpoint
                daemon.after_registering_state_commit
                daemon.after_register_request_write
                daemon.after_register_response_receive
                server.after_listener_established)
            string(REPLACE "." "-" failpoint_test_name "${failpoint}")
            add_test(NAME "fault.${failpoint_test_name}"
                COMMAND
                    "${CMAKE_CURRENT_SOURCE_DIR}/integration/fault_injection_reconciliation.sh"
                    $<TARGET_FILE:minitun>
                    $<TARGET_FILE:minitund>
                    $<TARGET_FILE:minitun-server>
                    "${failpoint}"
            )
            set_tests_properties("fault.${failpoint_test_name}" PROPERTIES
                LABELS "integration;fault-injection;reconciliation"
                TIMEOUT 60
                PASS_REGULAR_EXPRESSION "fault injection reconciliation passed"
                RUN_SERIAL TRUE
            )
        endforeach()
    endif()

    add_test(NAME integration.cli-daemon-ipc
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_daemon_ipc.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
    )
    set_tests_properties(integration.cli-daemon-ipc PROPERTIES
        LABELS "integration;ipc"
        TIMEOUT 20
        PASS_REGULAR_EXPRESSION "CLI/daemon IPC integration passed"
    )

    add_test(NAME integration.tls-auth
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/tls_auth.sh"
            $<TARGET_FILE:minitun-server>
            $<TARGET_FILE:minitun_tls_auth_client>
    )

    add_test(NAME integration.admin-endpoints
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/admin_endpoints.sh"
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.admin-endpoints PROPERTIES
        LABELS "integration;admin;metrics"
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "admin endpoint integration passed"
    )
    set_tests_properties(integration.tls-auth PROPERTIES
        LABELS "integration;tls;auth"
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "TLS authentication integration passed"
    )

    add_test(NAME integration.multi-server-sessions
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/multi_server_sessions.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.multi-server-sessions PROPERTIES
        LABELS "integration;tls;multi-server"
        TIMEOUT 45
        PASS_REGULAR_EXPRESSION "multi-server session integration passed"
    )

    add_test(NAME integration.tunnel-registration
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/tunnel_registration.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.tunnel-registration PROPERTIES
        LABELS "integration;tls;tunnel"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "tunnel registration integration passed"
    )

    add_test(NAME integration.high-latency-reconciliation
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/high_latency_reconciliation.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.high-latency-reconciliation PROPERTIES
        LABELS "integration;tls;tunnel;latency"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "high-latency tunnel reconciliation integration passed"
    )

    add_test(NAME integration.credential-rotation
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/credential_rotation.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.credential-rotation PROPERTIES
        LABELS "integration;tls;auth"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "credential rotation integration passed"
    )

    add_test(NAME integration.policy-reload
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/policy_reload.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.policy-reload PROPERTIES
        LABELS "integration;tls;auth;relay;policy"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "client policy reload integration passed"
    )

    add_test(NAME integration.worker-pool
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/worker_pool.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.worker-pool PROPERTIES
        LABELS "integration;tls;worker"
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "Worker Pool integration passed"
    )

    add_test(NAME integration.tcp-relay
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/tcp_relay.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.tcp-relay PROPERTIES
        LABELS "integration;e2e;tls;relay"
        TIMEOUT 90
        PASS_REGULAR_EXPRESSION "TCP relay integration passed"
    )

    add_test(NAME integration.stability
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/stability.sh"
            $<TARGET_FILE:minitun>
            $<TARGET_FILE:minitund>
            $<TARGET_FILE:minitun-server>
    )
    set_tests_properties(integration.stability PROPERTIES
        LABELS "integration;e2e;tls;stability"
        TIMEOUT 90
        PASS_REGULAR_EXPRESSION "stability integration passed"
    )

    add_test(NAME integration.install-layout
        COMMAND
            "${CMAKE_COMMAND}"
            "-DMINITUN_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMINITUN_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DMINITUN_LIBEXECDIR=${CMAKE_INSTALL_LIBEXECDIR}"
            "-DMINITUN_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DMINITUN_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
            "-DMINITUN_MANDIR=${CMAKE_INSTALL_MANDIR}"
            "-DMINITUN_SYSCONFDIR=${CMAKE_INSTALL_SYSCONFDIR}"
            "-DMINITUN_SYSTEMD_UNIT_DIR=${MINITUN_SYSTEMD_UNIT_DIR}"
            "-DMINITUN_SYSUSERS_DIR=${MINITUN_SYSUSERS_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/install_layout.cmake"
    )
    set_tests_properties(integration.install-layout PROPERTIES
        LABELS "integration;install"
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "component installation layout passed"
    )
endif()
