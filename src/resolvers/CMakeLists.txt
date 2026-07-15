foreach(suffix "" "-asan")
  set(target "omni-resolver${suffix}")

  add_library(${target} STATIC
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

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-base${suffix}
    omni-fiber${suffix}
    PkgConfig::CARES
  )

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
