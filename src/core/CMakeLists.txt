if(WIN32)
  add_subdirectory(windows)
endif()

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target_core "great-hole-core${suffix}")

  set(core_sources
    BackoffTimer.cpp
    Pipeline.cpp
    Endpoint.cpp
    Filter.cpp
    EndpointUdp.cpp
    EndpointUdpMux.cpp
    EndpointUdpDynMux.cpp
    UtilConsole.cpp
  )

  if(WIN32)
    list(APPEND core_sources
      windows/EndpointWinDivert.cpp
    )
  else()
    list(APPEND core_sources
      EndpointTun.cpp
      EndpointTunSplitIp.cpp
    )
  endif()

  add_library(${target_core} STATIC ${core_sources})

  target_include_directories(${target_core} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  if(WIN32)
    target_include_directories(${target_core} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/windows)
  endif()

  target_link_libraries(${target_core} PUBLIC
    Boost::log
    Boost::program_options
    Boost::filesystem
    Boost::thread
    omni-fiber${suffix}
    omni-resolver${suffix}
    great-hole-interface${suffix}
  )

  if(WIN32)
    target_link_libraries(${target_core} PUBLIC windivert_lib)
  endif()

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
