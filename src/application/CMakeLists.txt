include(FetchContent)

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(omnifiber "omni-fiber-asan")
    set(target "great-hole-application-asan")
  else()
    set(omnifiber "omni-fiber")
    set(target "great-hole-application")
  endif()

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

  if(WIN32 AND suffix STREQUAL "")
    add_library(${target} SHARED ${app_sources})
    target_compile_definitions(${target} PRIVATE GH_WINDOWS_EXPORT)
  else()
    add_library(${target} STATIC ${app_sources})
  endif()

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    ${omnifiber}
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
