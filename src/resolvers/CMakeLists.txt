foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(target "omni-resolver-asan")
    set(omnibase "omni-base-asan")
    set(omnifiber "omni-fiber-asan")
  else()
    set(target "omni-resolver")
    set(omnibase "omni-base")
    set(omnifiber "omni-fiber")
  endif()

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
    Threads::Threads
    Boost::log
    ${omnibase}
    ${omnifiber}
    PkgConfig::CARES
  )

  if (Boost_PROCESS_FOUND)
    target_link_libraries(${target} PUBLIC Boost::process)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
