foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "dns-forwarder${suffix}")

  add_executable(${target} DnsForwarderTool.cpp)

  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PRIVATE
    omni-dns${suffix}
    omni-fiber${suffix}
    omni-base${suffix}
    great-hole-interface${suffix}
    Boost::log
    Boost::thread
  )

  if(MSVC)
    target_link_options(${target} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()
