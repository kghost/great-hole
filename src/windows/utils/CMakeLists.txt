foreach(suffix "" "-asan")
  set(target "great-hole-windows-utils${suffix}")

  set(sources
    Strings.cpp
  )

  add_library(${target} STATIC ${sources})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()
