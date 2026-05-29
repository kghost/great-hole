# src/init.lua custom command
set(INIT_LUA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/init.lua)
set(INIT_LUA_OBJ ${CMAKE_CURRENT_BINARY_DIR}/init.lua.bin.o)

add_custom_command(
    OUTPUT ${INIT_LUA_OBJ}
    COMMAND ld -r -o ${INIT_LUA_OBJ} -z noexecstack --format=binary init.lua
    COMMAND objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents ${INIT_LUA_OBJ}
    DEPENDS ${INIT_LUA_SRC}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan")
    set(target_core "great-hole-core-asan")
    set(target_exe "great-hole-asan")
    set(lib_omnifiber "omnifiber-asan")
  else()
    set(target_core "great-hole-core")
    set(target_exe "great-hole")
    set(lib_omnifiber "omnifiber")
  endif()

  add_library(${target_core} STATIC
      ${INIT_LUA_OBJ}
      LuaLib.cpp
      LuaInterface.cpp
      Pipeline.cpp
      ServiceBase.cpp
      Endpoint.cpp
      ErrorCode.cpp
      Filter.cpp
      UtilConsole.cpp
      EndpointTun.cpp
      EndpointUdp.cpp
      EndpointUdpMuxServer.cpp
      EndpointUdpMuxClient.cpp
      EndpointUdpDynMuxServer.cpp
      EndpointUdpDynMuxClient.cpp
      LuaEngine.cpp
  )

  add_executable(${target_exe}
      main.cpp
  )

  target_include_directories(${target_core} PUBLIC
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_SOURCE_DIR}/libs
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

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
      PkgConfig::LUA
      ${lib_omnifiber}
  )

  if (Boost_PROCESS_FOUND)
      target_link_libraries(${target_core} PUBLIC Boost::process)
  endif()

  target_link_libraries(${target_exe} PRIVATE
      ${target_core}
  )

  if(suffix STREQUAL "-asan")
      target_compile_options(${target_core} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target_core} PRIVATE -fsanitize=address)
      target_compile_options(${target_exe} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target_exe} PRIVATE -fsanitize=address)
  endif()

  include(GNUInstallDirs)
  install(TARGETS ${target_exe} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
