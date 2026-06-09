include(GoogleTest)

add_executable(resolver_test TestResolver.cpp)
target_link_libraries(resolver_test PRIVATE omni-fiber-asan omni-resolver-asan GTest::gtest_main)
target_compile_options(resolver_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(resolver_test PRIVATE -fsanitize=address)
gtest_discover_tests(resolver_test)
