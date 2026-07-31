add_executable(minitun_unit_tests
    unit/common_test.cpp
    unit/endpoint_test.cpp
    unit/id_test.cpp
    unit/logging_test.cpp
    unit/port_range_test.cpp
    unit/secure_string_test.cpp
    unit/state_recovery_test.cpp
    unit/storage_database_test.cpp
    unit/storage_models_test.cpp
    unit/storage_repository_test.cpp
    unit/time_test.cpp
)

target_link_libraries(minitun_unit_tests
    PRIVATE
        MiniTun::storage
        MiniTun::sqlite
        GTest::gtest_main
        nlohmann_json::nlohmann_json
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_unit_tests)
minitun_apply_lto(minitun_unit_tests)

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
