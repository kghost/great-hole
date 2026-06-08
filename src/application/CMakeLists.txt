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
    great-hole-core${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()
