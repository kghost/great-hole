include(FetchContent)

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "great-hole-application${suffix}")

  set(app_sources
    VpnClientMultiChannel.cpp
    ConnectionTracker.cpp
    TunnelDataPlane.cpp
  )

  if(NOT WIN32)
    list(APPEND app_sources
      VpnServer.cpp
    )
  endif()

  add_library(${target} STATIC ${app_sources})

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-fiber${suffix}
    great-hole-interface${suffix}
    great-hole-core${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
