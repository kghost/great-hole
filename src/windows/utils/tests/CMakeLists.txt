include(GoogleTest)

add_executable(windows_utils_test
  TestAutoHandle.cpp
)
target_link_libraries(windows_utils_test PRIVATE
  great-hole-windows-utils-asan
  GTest::gtest_main
)
target_compile_options(windows_utils_test PRIVATE -fsanitize=address)
target_link_options(windows_utils_test PRIVATE -fsanitize=address)

gtest_discover_tests(windows_utils_test DISCOVERY_MODE PRE_TEST)
