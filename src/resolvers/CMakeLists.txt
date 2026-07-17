foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "omni-resolver${suffix}")

  if(WIN32)
    set(resolver_sources
      Resolver.cpp
      ResolverStaticIp.cpp
      ResolverIpDns.cpp
      ResolverNumberPort.cpp
      ResolverServicePort.cpp
      ResolverCombinedEndpoint.cpp
      ResolverDnsService.cpp
      ResolverStaticEndpoint.cpp
      ResolverHelper.cpp
      WindowsAsyncResolver.cpp
    )
  else()
    set(resolver_sources
      Resolver.cpp
      ResolverStaticIp.cpp
      ResolverIpDns.cpp
      ResolverNumberPort.cpp
      ResolverServicePort.cpp
      ResolverCombinedEndpoint.cpp
      ResolverDnsService.cpp
      ResolverStaticEndpoint.cpp
      ResolverHelper.cpp
      AresResolver.cpp
    )
  endif()

  add_library(${target} STATIC ${resolver_sources})

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-base${suffix}
    omni-fiber${suffix}
  )

  if(WIN32)
    target_link_libraries(${target} PUBLIC iphlpapi dnsapi)
  else()
    target_link_libraries(${target} PUBLIC PkgConfig::CARES)
  endif()

  if (Boost_PROCESS_FOUND)
    target_link_libraries(${target} PUBLIC Boost::process)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
