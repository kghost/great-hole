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

add_library(great-hole-core STATIC
    lua-lib.cpp
    pipeline.cpp
    endpoint.cpp
    endpoint-tun.cpp
    endpoint-udp.cpp
    endpoint-udp-mux-server.cpp
    endpoint-udp-mux-client.cpp
    endpoint-udp-dyn-mux-server.cpp
    endpoint-udp-dyn-mux-client.cpp
    filter.cpp
    util-exec.cpp
    util-console.cpp
    logging.cpp
    error-code.cpp
    ${INIT_LUA_OBJ}
)

add_executable(great-hole
    main.cpp
)

target_include_directories(great-hole-core PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(great-hole-core PUBLIC
    BOOST_ALL_DYN_LINK
    BOOST_ASIO_ENABLE_OLD_SERVICES
    BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
    BOOST_ASIO_HAS_MOVE
)

target_link_libraries(great-hole-core PUBLIC
    Threads::Threads
    ZLIB::ZLIB
    Boost::log
    Boost::iostreams
    Boost::program_options
    Boost::filesystem
    Boost::thread
    PkgConfig::LUA
    omnifiber
)

if (Boost_PROCESS_FOUND)
    target_link_libraries(great-hole-core PUBLIC Boost::process)
endif()

target_link_libraries(great-hole PRIVATE
    great-hole-core
)

include(GNUInstallDirs)
install(TARGETS great-hole RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
