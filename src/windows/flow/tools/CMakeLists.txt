include(FetchContent)
FetchContent_GetProperties(windivert)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WINDIVERT_ARCH "x64")
  set(WINDIVERT_SYS "WinDivert64.sys")
else()
  set(WINDIVERT_ARCH "x86")
  set(WINDIVERT_SYS "WinDivert32.sys")
endif()

foreach(suffix "" "-asan")
  set(target "flow-dump${suffix}")

  add_executable(${target} FlowDump.cpp)

  target_link_libraries(${target} PRIVATE
    great-hole-windows-flow${suffix}
    great-hole-windows-utils${suffix}
    Boost::program_options
    Boost::log
  )

  if(MSVC)
    target_link_options(${target} PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()

  # Copy real WinDivert.dll and WinDivert64.sys/WinDivert32.sys post-build
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${windivert_SOURCE_DIR}/${WINDIVERT_ARCH}/WinDivert.dll"
        "$<TARGET_FILE_DIR:${target}>/WinDivert.dll"
    )
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${windivert_SOURCE_DIR}/${WINDIVERT_ARCH}/${WINDIVERT_SYS}"
        "$<TARGET_FILE_DIR:${target}>/${WINDIVERT_SYS}"
    )
  endif()
endforeach()
