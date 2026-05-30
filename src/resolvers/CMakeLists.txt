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
  )

  target_include_directories(${target} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_SOURCE_DIR}/src
  )

  target_compile_definitions(${target} PUBLIC
    BOOST_ALL_DYN_LINK
    BOOST_ASIO_ENABLE_OLD_SERVICES
    BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
    BOOST_ASIO_HAS_MOVE
  )

  target_link_libraries(${target} PUBLIC
    Threads::Threads
    Boost::log
    ${omnibase}
    ${omnifiber}
    resolv
  )

  if (Boost_PROCESS_FOUND)
    target_link_libraries(${target} PUBLIC Boost::process)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

add_subdirectory(tests)
