cmake_minimum_required(VERSION 3.10)
project(great-hole VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS log iostreams program_options filesystem thread process)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua-5.4)

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

add_library(great-hole-core STATIC
    src/lua-lib.cpp
    src/pipeline.cpp
    src/endpoint.cpp
    src/endpoint-tun.cpp
    src/endpoint-udp.cpp
    src/endpoint-udp-mux-server.cpp
    src/endpoint-udp-mux-client.cpp
    src/endpoint-udp-dyn-mux-server.cpp
    src/endpoint-udp-dyn-mux-client.cpp
    src/filter.cpp
    src/util-exec.cpp
    src/util-console.cpp
    src/logging.cpp
    src/error-code.cpp
    ${INIT_LUA_OBJ}
)

add_executable(great-hole
    src/main.cpp
)

target_include_directories(great-hole-core PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    Boost::process
    PkgConfig::LUA
)

target_link_libraries(great-hole PRIVATE
    great-hole-core
)

enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

add_subdirectory(tests)
