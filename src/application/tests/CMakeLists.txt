include(GoogleTest)

add_executable(vpn_server_test TestVpnServer.cpp TestVpnClientMultiChannel.cpp TestConnectionTracker.cpp)
target_link_libraries(vpn_server_test PRIVATE
  great-hole-application-asan
  great-hole-core-asan
  GTest::gtest_main
)

target_compile_options(vpn_server_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(vpn_server_test PRIVATE -fsanitize=address)
gtest_discover_tests(vpn_server_test)
