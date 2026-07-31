add_executable(minitun_unit_tests
    unit/common_test.cpp
    unit/credential_store_test.cpp
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
    unit/remote_state_machine_test.cpp
    unit/reconnect_backoff_test.cpp
    unit/server_session_registry_test.cpp
    unit/secure_string_test.cpp
    unit/state_recovery_test.cpp
    unit/storage_database_test.cpp
    unit/storage_models_test.cpp
    unit/storage_repository_test.cpp
    unit/time_test.cpp
    unit/tunnel_registry_test.cpp
    unit/worker_pool_test.cpp
)

target_link_libraries(minitun_unit_tests
    PRIVATE
        MiniTun::daemon
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
    PASS_REGULAR_EXPRESSION "protocol version: 1"
)

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
endif()
