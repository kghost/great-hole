foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(target "omni-base-asan")
    set(omnifiber "omni-fiber-asan")
  else()
    set(target "omni-base")
    set(omnifiber "omni-fiber")
  endif()

  add_library(${target} STATIC
    ServiceBase.cpp
    ErrorCode.cpp
  )

  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_compile_definitions(${target} PUBLIC
    BOOST_ALL_DYN_LINK
    BOOST_ASIO_ENABLE_OLD_SERVICES
    BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
    BOOST_ASIO_HAS_MOVE
  )

  target_link_libraries(${target} PUBLIC
    Boost::log
    ${omnifiber}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

