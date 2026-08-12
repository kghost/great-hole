include(GoogleTest)

add_executable(dns_test DnsForwarderTest.cpp)
target_link_libraries(dns_test PRIVATE omni-fiber-asan omni-dns-asan omni-base-asan GTest::gtest_main)
target_compile_options(dns_test PRIVATE -fsanitize=address)
target_link_options(dns_test PRIVATE -fsanitize=address)
gtest_discover_tests(dns_test)
