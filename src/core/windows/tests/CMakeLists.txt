include(GoogleTest)

# Create the fake WinDivert DLL target
add_library(fake_windivert SHARED FakeWinDivertDll.cpp)
target_include_directories(fake_windivert PRIVATE "${windivert_SOURCE_DIR}/include")
target_link_libraries(fake_windivert PRIVATE ws2_32)
set_target_properties(fake_windivert PROPERTIES OUTPUT_NAME "WinDivert")

# Create the test executable
add_executable(windivert_test TestEndpointWinDivert.cpp FakeWinDivert.cpp)
target_include_directories(windivert_test PRIVATE "${windivert_SOURCE_DIR}/include")
target_link_libraries(windivert_test PRIVATE
  great-hole-core-asan
  GTest::gtest_main
)
target_compile_options(windivert_test PRIVATE -fsanitize=address)
target_link_options(windivert_test PRIVATE -fsanitize=address)

# Copy our fake WinDivert.dll to the test directory post-build
add_custom_command(TARGET windivert_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:fake_windivert>"
    "$<TARGET_FILE_DIR:windivert_test>/WinDivert.dll"
)

gtest_discover_tests(windivert_test)
