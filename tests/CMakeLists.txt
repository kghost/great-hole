include(GoogleTest)

add_executable(udp_mux_test test_udp_mux.cpp)
target_link_libraries(udp_mux_test PRIVATE great-hole-core gtest_main)
gtest_discover_tests(udp_mux_test)

add_executable(udp_dyn_mux_test test_udp_dyn_mux.cpp)
target_link_libraries(udp_dyn_mux_test PRIVATE great-hole-core gtest_main)
gtest_discover_tests(udp_dyn_mux_test)
