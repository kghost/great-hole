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

# Headers Check
include(CheckIncludeFileCXX)
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)

check_include_file_cxx(alloca.h HAVE_ALLOCA_H)
check_include_file_cxx(dirent.h HAVE_DIRENT_H)
check_include_file_cxx(dlfcn.h HAVE_DLFCN_H)
check_include_file_cxx(fcntl.h HAVE_FCNTL_H)
check_include_file_cxx(inttypes.h HAVE_INTTYPES_H)
check_include_file_cxx(locale.h HAVE_LOCALE_H)
check_include_file_cxx(memory.h HAVE_MEMORY_H)
check_include_file_cxx(stdint.h HAVE_STDINT_H)
check_include_file_cxx(stdio.h HAVE_STDIO_H)
check_include_file_cxx(stdlib.h HAVE_STDLIB_H)
check_include_file_cxx(strings.h HAVE_STRINGS_H)
check_include_file_cxx(string.h HAVE_STRING_H)
check_include_file_cxx(sys/param.h HAVE_SYS_PARAM_H)
check_include_file_cxx(sys/stat.h HAVE_SYS_STAT_H)
check_include_file_cxx(sys/sysmacros.h HAVE_SYS_SYSMACROS_H)
check_include_file_cxx(sys/time.h HAVE_SYS_TIME_H)
check_include_file_cxx(sys/types.h HAVE_SYS_TYPES_H)
check_include_file_cxx(time.h HAVE_TIME_H)
check_include_file_cxx(unistd.h HAVE_UNISTD_H)

check_function_exists(alloca HAVE_ALLOCA)

set(BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS 1)
set(BOOST_ASIO_HAS_MOVE 1)
set(HAVE_BOOST 1)
set(HAVE_BOOST_FILESYSTEM 1)
set(HAVE_BOOST_IOSTREAMS 1)
set(HAVE_BOOST_LOG 1)
set(HAVE_BOOST_PROGRAM_OPTIONS 1)
set(HAVE_BOOST_THREAD 1)
set(HAVE_CXX14 1)
set(HAVE_PTHREAD 1)
set(HAVE_PTHREAD_PRIO_INHERIT 1)
set(HAVE_STRUCT_STAT_ST_BLOCKS 1)
set(HAVE_ST_BLOCKS 1)
set(LT_OBJDIR ".libs/")
set(MAJOR_IN_SYSMACROS 1)
set(PACKAGE ${PROJECT_NAME})
set(PACKAGE_BUGREPORT "")
set(PACKAGE_NAME ${PROJECT_NAME})
set(PACKAGE_STRING "${PROJECT_NAME} ${PROJECT_VERSION}")
set(PACKAGE_TARNAME ${PROJECT_NAME})
set(PACKAGE_URL "")
set(PACKAGE_VERSION ${PROJECT_VERSION})
set(STDC_HEADERS 1)
set(VERSION ${PROJECT_VERSION})

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
