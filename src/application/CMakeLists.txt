foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(omnifiber "omni-fiber-asan")
    set(target "great-hole-application-asan")
  else()
    set(omnifiber "omni-fiber")
    set(target "great-hole-application")
  endif()

  add_library(${target} STATIC
    VpnServer.cpp
    VpnClientMultiChannel.cpp
    ConnectionTracker.cpp
  )

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_link_libraries(${target} PUBLIC
    Boost::log
    ${omnifiber}
    great-hole-core${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
