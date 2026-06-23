include(GoogleTest)

add_executable(udp_test TestUdp.cpp)
target_link_libraries(udp_test PRIVATE great-hole-core-asan GTest::gtest_main)
target_compile_options(udp_test PRIVATE -fsanitize=address)
target_link_options(udp_test PRIVATE -fsanitize=address)
gtest_discover_tests(udp_test)

add_executable(udp_mux_test TestUdpMux.cpp)
target_link_libraries(udp_mux_test PRIVATE great-hole-core-asan GTest::gtest_main)
target_compile_options(udp_mux_test PRIVATE -fsanitize=address)
target_link_options(udp_mux_test PRIVATE -fsanitize=address)
gtest_discover_tests(udp_mux_test)

add_executable(udp_dyn_mux_test TestUdpDynMux.cpp)
target_link_libraries(udp_dyn_mux_test PRIVATE great-hole-core-asan GTest::gtest_main)
target_compile_options(udp_dyn_mux_test PRIVATE -fsanitize=address)
target_link_options(udp_dyn_mux_test PRIVATE -fsanitize=address)
gtest_discover_tests(udp_dyn_mux_test)

if(NOT WIN32)
add_executable(tun_split_ip_test TestTunSplitIp.cpp)
target_link_libraries(tun_split_ip_test PRIVATE great-hole-core-asan GTest::gtest_main)
target_compile_options(tun_split_ip_test PRIVATE -fsanitize=address)
target_link_options(tun_split_ip_test PRIVATE -fsanitize=address)
gtest_discover_tests(tun_split_ip_test)
endif()

add_executable(packet_builder_test TestPacketBuilder.cpp)
target_link_libraries(packet_builder_test PRIVATE great-hole-core-asan GTest::gtest_main)
target_compile_options(packet_builder_test PRIVATE -fsanitize=address)
target_link_options(packet_builder_test PRIVATE -fsanitize=address)
gtest_discover_tests(packet_builder_test)

