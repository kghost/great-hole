add_subdirectory(utils)
add_subdirectory(flow)
add_subdirectory(policy)

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "great-hole-windows${suffix}")

  add_library(${target} SHARED
    Interface.cpp
  )
  target_compile_definitions(${target} PRIVATE GREAT_HOLE_WINDOWS_BUILD_DLL)
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target} PUBLIC
    Boost::log
    omni-fiber${suffix}
    great-hole-core${suffix}
    great-hole-application${suffix}
    great-hole-windows-policy${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()
