foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "omni-dns${suffix}")

  set(dns_sources
    DnsPacket.cpp
    DnsRouter.cpp
    DnsUpstream.cpp
    DnsListener.cpp
    DnsForwarder.cpp
  )

  add_library(${target} STATIC ${dns_sources})

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-base${suffix}
    omni-fiber${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
