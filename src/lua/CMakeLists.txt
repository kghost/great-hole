# src/init.lua custom command
set(INIT_LUA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/init.lua)
set(INIT_LUA_OBJ ${CMAKE_CURRENT_BINARY_DIR}/init.lua.bin.o)

add_custom_command(
  OUTPUT ${INIT_LUA_OBJ}
  COMMAND ${CMAKE_LINKER} -r -o ${INIT_LUA_OBJ} -z noexecstack --format=binary init.lua
  COMMAND ${CMAKE_OBJCOPY} --rename-section .data=.rodata,alloc,load,readonly,data,contents ${INIT_LUA_OBJ}
  DEPENDS ${INIT_LUA_SRC}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

foreach(suffix "" "-asan")
  set(target_exe "great-hole${suffix}")

  add_executable(${target_exe}
    ${INIT_LUA_OBJ}
    LuaEngine.cpp
    LuaInterface.cpp
    LuaLib.cpp
    LuaLibEndpoint.cpp
    LuaLibFilter.cpp
    LuaLibPipeline.cpp
    LuaLibUdp.cpp
    LuaLibUdpDynMux.cpp
    LuaLibUdpMux.cpp
    LuaLibTunSplitIp.cpp
    LuaLibVpnServer.cpp
    main.cpp
  )

  target_link_libraries(${target_exe} PRIVATE
    Boost::log
    Boost::program_options
    Boost::filesystem
    Boost::thread
    PkgConfig::LUA
    omni-fiber${suffix}
    omni-resolver${suffix}
    great-hole-core${suffix}
    great-hole-application${suffix}
  )

  if(suffix STREQUAL "-asan")
    target_compile_options(${target_exe} PRIVATE -fsanitize=address)
    target_link_options(${target_exe} PRIVATE -fsanitize=address)
  endif()

  include(GNUInstallDirs)
  install(TARGETS ${target_exe} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
