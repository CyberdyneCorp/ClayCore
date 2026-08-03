add_executable(clay_unit_tests
  unit/test_version.cpp)

target_link_libraries(clay_unit_tests PRIVATE clay::core doctest::doctest)
clay_apply_warnings(clay_unit_tests)
clay_apply_sanitizers(clay_unit_tests)

add_test(NAME clay_unit_tests COMMAND clay_unit_tests)
