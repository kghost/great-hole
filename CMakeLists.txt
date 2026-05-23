cmake_minimum_required(VERSION 3.10)
project(great-hole VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS log iostreams program_options filesystem thread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua-5.4)

set(BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS 1)
set(BOOST_ASIO_HAS_MOVE 1)

configure_file(config.h.in config.h)

# src/init.lua custom command
set(INIT_LUA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/init.lua)
set(INIT_LUA_OBJ ${CMAKE_CURRENT_BINARY_DIR}/init.lua.bin.o)

add_custom_command(
    OUTPUT ${INIT_LUA_OBJ}
    COMMAND ld -r -o ${INIT_LUA_OBJ} -z noexecstack --format=binary init.lua
    COMMAND objcopy --rename-section .data=.rodata,alloc,load,readonly,data,contents ${INIT_LUA_OBJ}
    DEPENDS ${INIT_LUA_SRC}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(great-hole
    src/main.cpp
    src/lua-lib.cpp
    src/pipeline.cpp
    src/endpoint.cpp
    src/endpoint-tun.cpp
    src/endpoint-udp.cpp
    src/endpoint-udp-mux-server.cpp
    src/endpoint-udp-mux-client.cpp
    src/filter.cpp
    src/util-exec.cpp
    src/util-console.cpp
    src/logging.cpp
    src/error-code.cpp
    ${INIT_LUA_OBJ}
)

target_include_directories(great-hole PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(great-hole PRIVATE
    BOOST_ALL_DYN_LINK
    BOOST_ASIO_ENABLE_OLD_SERVICES
)

target_link_libraries(great-hole PRIVATE
    Threads::Threads
    ZLIB::ZLIB
    Boost::log
    Boost::iostreams
    Boost::program_options
    Boost::filesystem
    Boost::thread
    PkgConfig::LUA
)
