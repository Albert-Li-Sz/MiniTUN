add_executable(minitun_unit_tests
    unit/common_test.cpp
)

target_link_libraries(minitun_unit_tests
    PRIVATE
        MiniTun::common
        GTest::gtest_main
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
