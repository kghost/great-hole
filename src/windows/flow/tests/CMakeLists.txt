include(GoogleTest)

# Create the test executable
add_executable(flow_sniffer_test
  TestWinDivertFlowSniffer.cpp
  ../../../core/windows/tests/FakeWinDivert.cpp
)

target_include_directories(flow_sniffer_test PRIVATE
  "${windivert_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/windows/tests"
)

target_link_libraries(flow_sniffer_test PRIVATE
  great-hole-windows-flow-asan
  GTest::gtest_main
)

target_compile_options(flow_sniffer_test PRIVATE -fsanitize=address)
target_link_options(flow_sniffer_test PRIVATE -fsanitize=address)

# Copy our fake WinDivert.dll to the test directory post-build
add_custom_command(TARGET flow_sniffer_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:fake_windivert>"
    "$<TARGET_FILE_DIR:flow_sniffer_test>/WinDivert.dll"
)

gtest_discover_tests(flow_sniffer_test)
