foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(target_core "great-hole-core-asan")
    set(omnifiber "omni-fiber-asan")
    set(omniresolver "omni-resolver-asan")
  else()
    set(target_core "great-hole-core")
    set(omnifiber "omni-fiber")
    set(omniresolver "omni-resolver")
  endif()

  set(core_sources
    BackoffTimer.cpp
    Pipeline.cpp
    Endpoint.cpp
    Filter.cpp
    UtilConsole.cpp
    EndpointUdp.cpp
    EndpointUdpMux.cpp
    EndpointUdpDynMux.cpp
  )

  if(NOT WIN32)
    list(APPEND core_sources
      EndpointTun.cpp
      EndpointTunSplitIp.cpp
    )
  endif()

  add_library(${target_core} STATIC ${core_sources})

  target_include_directories(${target_core} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target_core} PUBLIC
    Boost::log
    Boost::program_options
    Boost::filesystem
    Boost::thread
    ${omnifiber}
    ${omniresolver}
  )

  if(Boost_PROCESS_FOUND)
    target_link_libraries(${target_core} PUBLIC Boost::process)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target_core} PRIVATE -fsanitize=address)
    target_link_options(${target_core} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
