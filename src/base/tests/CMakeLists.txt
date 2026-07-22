include(GoogleTest)

add_executable(base_test TestServiceBase.cpp TestCancel.cpp TestStateMachine.cpp)
target_link_libraries(base_test PRIVATE omni-fiber-asan omni-base-asan GTest::gtest_main)
target_compile_options(base_test PRIVATE -fsanitize=address)
target_link_options(base_test PRIVATE -fsanitize=address)
gtest_discover_tests(base_test)
