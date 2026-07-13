include(GoogleTest)

add_executable(policy_engine_test
  TestFlowTracker.cpp
  TestPolicyRegistry.cpp
  TestPolicySelector.cpp
  TestProcessTreeTracker.cpp
)
target_link_libraries(policy_engine_test PRIVATE
  great-hole-windows-policy-asan
  great-hole-application-asan
  great-hole-core-asan
  GTest::gtest
)
target_compile_options(policy_engine_test PRIVATE -fsanitize=address)
target_link_options(policy_engine_test PRIVATE -fsanitize=address)

gtest_discover_tests(policy_engine_test DISCOVERY_MODE PRE_TEST)
