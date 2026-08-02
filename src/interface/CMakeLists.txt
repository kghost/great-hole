execute_process(
  COMMAND git describe --tags
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GREAT_HOLE_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE GIT_RESULT
)

if(NOT GIT_RESULT EQUAL 0 OR GREAT_HOLE_VERSION STREQUAL "")
  set(GREAT_HOLE_VERSION "unknown")
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/Version.h
  @ONLY
)

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "great-hole-interface${suffix}")
  add_library(${target} STATIC
    InterfaceVersion.cpp
  )
  target_include_directories(${target} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()
