include(GoogleTest)

add_executable(vpn_connection_tracker_test TestConnectionTracker.cpp)
target_link_libraries(vpn_connection_tracker_test PRIVATE great-hole-application-asan great-hole-core-asan omni-timetravel GTest::gtest)
target_compile_options(vpn_connection_tracker_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(vpn_connection_tracker_test PRIVATE -fsanitize=address)
gtest_discover_tests(vpn_connection_tracker_test)

add_executable(vpn_client_test TestVpnClientMultiChannel.cpp)
target_link_libraries(vpn_client_test PRIVATE great-hole-application-asan great-hole-core-asan omni-timetravel GTest::gtest)
target_compile_options(vpn_client_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(vpn_client_test PRIVATE -fsanitize=address)
gtest_discover_tests(vpn_client_test)

add_executable(vpn_server_test TestVpnServer.cpp)
target_link_libraries(vpn_server_test PRIVATE great-hole-application-asan great-hole-core-asan GTest::gtest_main)
target_compile_options(vpn_server_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(vpn_server_test PRIVATE -fsanitize=address)
gtest_discover_tests(vpn_server_test)
