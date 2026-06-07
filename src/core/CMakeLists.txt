foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(target_core "great-hole-core-asan")
    set(omnifiber "omni-fiber-asan")
    set(omniresolver "omni-resolver-asan")
  else()
    set(target_core "great-hole-core")
    set(omnifiber "omni-fiber")
    set(omniresolver "omni-resolver")
  endif()

  add_library(${target_core} STATIC
    BackoffTimer.cpp
    Pipeline.cpp
    Endpoint.cpp
    Filter.cpp
    UtilConsole.cpp
    EndpointTun.cpp
    EndpointUdp.cpp
    EndpointUdpMux.cpp
    EndpointUdpDynMux.cpp
  )

  target_include_directories(${target_core} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

  target_compile_definitions(${target_core} PUBLIC
    BOOST_ALL_DYN_LINK
    BOOST_ASIO_ENABLE_OLD_SERVICES
    BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
    BOOST_ASIO_HAS_MOVE
  )

  target_link_libraries(${target_core} PUBLIC
    Threads::Threads
    ZLIB::ZLIB
    Boost::log
    Boost::program_options
    Boost::filesystem
    Boost::thread
    ${omnifiber}
    ${omniresolver}
  )

  if(Boost_PROCESS_FOUND)
    target_link_libraries(${target_core} PUBLIC Boost::process)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target_core} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target_core} PRIVATE -fsanitize=address)
  endif()
endforeach()

add_subdirectory(tests)
