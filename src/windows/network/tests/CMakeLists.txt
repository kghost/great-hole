include(GoogleTest)

add_executable(interface_monitor_test
  TestInterfaceMonitor.cpp
)

target_link_libraries(interface_monitor_test PRIVATE
  great-hole-windows-network-asan
  great-hole-windows-utils-asan
  omni-fiber-asan
  great-hole-core-asan
  great-hole-application-asan
  GTest::gtest_main
)

target_compile_options(interface_monitor_test PRIVATE -fsanitize=address)
target_link_options(interface_monitor_test PRIVATE -fsanitize=address)

gtest_discover_tests(interface_monitor_test DISCOVERY_MODE PRE_TEST)
