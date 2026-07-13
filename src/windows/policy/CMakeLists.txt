foreach(suffix "" "-asan")
  set(target "great-hole-windows-policy${suffix}")

  set(win_sources
    FlowTracker.cpp
    PolicyRegistry.cpp
    PolicySelector.cpp
    PolicyEngine.cpp
    ProcessTreeTracker.cpp
  )

  add_library(${target} STATIC ${win_sources})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-fiber${suffix}
    great-hole-interface${suffix}
    great-hole-core${suffix}
    great-hole-application${suffix}
    great-hole-windows-utils${suffix}
    great-hole-windows-flow${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
